#include "term.h"
#include "prompt.h"
#include "config.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

char history[MAX_HISTORY][MAX_LINE];
int history_count = 0;
int old_cursor_rows = 0;

void enable_raw_mode(struct termios *orig) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, orig);
    raw = *orig;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_oflag |= (OPOST | ONLCR);
    tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
    write(1, "\033[?25h", 6);
}

void disable_raw_mode(struct termios *orig) {
    tcsetattr(STDIN_FILENO, TCSADRAIN, orig);
}

static int prev_u8_char(const char *buf, int cursor) {
    if (cursor <= 0) return 0;
    int i = cursor - 1;
    while (i > 0 && (buf[i] & 0xC0) == 0x80) {
        i--;
    }
    return i;
}

static int next_u8_char(const char *buf, int cursor, int len) {
    if (cursor >= len) return len;
    int i = cursor + 1;
    while (i < len && (buf[i] & 0xC0) == 0x80) {
        i++;
    }
    return i;
}

static int u8_visual_width(const char *str, int bytes) {
    int width = 0;
    int i = 0;
    while (i < bytes) {
        width++;
        i = next_u8_char(str, i, bytes);
    }
    return width;
}

static int is_word_break(char c) {
    return c == ' ' || c == '\t' || c == '/' || c == '.' || c == '-'
        || c == '_' || c == '=' || c == ',' || c == ';' || c == '|'
        || c == '&' || c == '>' || c == '<' || c == '(' || c == ')'
        || c == '[' || c == ']' || c == '{' || c == '}' || c == '"'
        || c == '\'' || c == '`' || c == '$' || c == '!' || c == '#'
        || c == '~' || c == '+';
}

static int find_prev_word_start(const char *buf, int cursor) {
    if (cursor <= 0) return 0;
    int i = prev_u8_char(buf, cursor);
    while (i > 0 && is_word_break(buf[i])) i = prev_u8_char(buf, i);
    while (i > 0) {
        int p = prev_u8_char(buf, i);
        if (is_word_break(buf[p])) break;
        i = p;
    }
    return i;
}

static int find_next_word_end(const char *buf, int len, int cursor) {
    if (cursor >= len) return len;
    int i = cursor;
    while (i < len && !is_word_break(buf[i])) i = next_u8_char(buf, i, len);
    while (i < len && is_word_break(buf[i])) i = next_u8_char(buf, i, len);
    return i;
}

static int find_next_word_start(const char *buf, int len, int cursor) {
    if (cursor >= len) return len;
    int i = cursor;
    while (i < len && !is_word_break(buf[i])) i = next_u8_char(buf, i, len);
    while (i < len && is_word_break(buf[i])) i = next_u8_char(buf, i, len);
    return i;
}

static void refresh_line(const char *buffer, int len, int cursor, int prompt_len) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) ws.ws_col = 80;
    
    int vis_len = u8_visual_width(buffer, len);
    int vis_cursor = u8_visual_width(buffer, cursor);
    
    if (old_cursor_rows > 0) {
        char seq[32];
        snprintf(seq, sizeof(seq), "\033[%dA", old_cursor_rows);
        write(1, seq, strlen(seq));
    }
    write(1, "\r", 1);
    
    print_prompt();
    write(1, buffer, len);
    write(1, "\033[J", 3);
    
    int end_row = (prompt_len + vis_len) / ws.ws_col;
    int cur_row = (prompt_len + vis_cursor) / ws.ws_col;
    int cur_col = (prompt_len + vis_cursor) % ws.ws_col;
    
    old_cursor_rows = cur_row;
    
    if (end_row > cur_row) {
        char seq[32];
        snprintf(seq, sizeof(seq), "\033[%dA", end_row - cur_row);
        write(1, seq, strlen(seq));
    }
    write(1, "\r", 1);
    if (cur_col > 0) {
        char seq[32];
        snprintf(seq, sizeof(seq), "\033[%dC", cur_col);
        write(1, seq, strlen(seq));
    }
}

static void do_tab_completion(char *buffer, int *len, int *cursor, int prompt_len) {
    int word_start = *cursor;
    while (word_start > 0 && buffer[word_start - 1] != ' ') {
        word_start--;
    }
    int old_word_len = *cursor - word_start;

    char word[512];
    strncpy(word, buffer + word_start, old_word_len);
    word[old_word_len] = '\0';

    int path_start = 0;
    char *eq = strchr(word, '=');
    if (eq) {
        path_start = (int)(eq - word + 1);
    }

    char dir_to_open[512] = ".";
    char match_prefix[256] = "";
    char *last_slash = strrchr(word + path_start, '/');

    if (last_slash) {
        int dir_len = (last_slash - word) + 1;
        strncpy(dir_to_open, word + path_start, dir_len - path_start);
        dir_to_open[dir_len - path_start] = '\0';
        strcpy(match_prefix, last_slash + 1);

        if (dir_to_open[0] == '~') {
            char *home = getenv("HOME");
            if (home) {
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "%s%s", home, dir_to_open + 1);
                strcpy(dir_to_open, tmp);
            }
        }
    } else {
        strcpy(match_prefix, word + path_start);
    }

    char matches[128][256];
    int match_count = 0;
    int used_custom_completions = 0;

    if (!last_slash && word_start == 0) {
        for (int i = 0; i < alias_count && match_count < 128; i++) {
            if (strncmp(aliases[i].name, match_prefix, strlen(match_prefix)) == 0) {
                int exists = 0;
                for (int k = 0; k < match_count; k++) {
                    if (strcmp(matches[k], aliases[i].name) == 0) { exists = 1; break; }
                }
                if (!exists) strncpy(matches[match_count++], aliases[i].name, 255);
            }
        }

        for (int i = 0; i < function_count && match_count < 128; i++) {
            if (strncmp(functions[i].name, match_prefix, strlen(match_prefix)) == 0) {
                int exists = 0;
                for (int k = 0; k < match_count; k++) {
                    if (strcmp(matches[k], functions[i].name) == 0) { exists = 1; break; }
                }
                if (!exists) strncpy(matches[match_count++], functions[i].name, 255);
            }
        }

        char *path_env = getenv("PATH");
        if (path_env) {
            char *path_copy = strdup(path_env);
            if (path_copy) {
                char *dir = strtok(path_copy, ":");
                while (dir && match_count < 128) {
                    DIR *d = opendir(dir);
                    if (d) {
                        struct dirent *ent;
                        while ((ent = readdir(d)) != NULL && match_count < 128) {
                            if (strncmp(ent->d_name, match_prefix, strlen(match_prefix)) == 0) {
                                if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                                    int exists = 0;
                                    for (int i = 0; i < match_count; i++) {
                                        if (strcmp(matches[i], ent->d_name) == 0) { exists = 1; break; }
                                    }
                                    if (!exists) strncpy(matches[match_count++], ent->d_name, 255);
                                }
                            }
                        }
                        closedir(d);
                    }
                    dir = strtok(NULL, ":");
                }
                free(path_copy);
            }
        }
    } else if (!last_slash && word_start > 0) {
        char first_word[512];
        int fw_len = 0;
        while (fw_len < word_start && buffer[fw_len] && !isspace((unsigned char)buffer[fw_len]))
            first_word[fw_len++] = buffer[fw_len];
        first_word[fw_len] = '\0';

        const char *comp_words = get_completions(first_word);
        if (comp_words && *comp_words) {
            char words_copy[1024];
            strncpy(words_copy, comp_words, sizeof(words_copy) - 1);
            words_copy[sizeof(words_copy) - 1] = '\0';
            char *w = strtok(words_copy, " ");
            while (w && match_count < 128) {
                if (strncmp(w, match_prefix, strlen(match_prefix)) == 0) {
                    int exists = 0;
                    for (int k = 0; k < match_count; k++) {
                        if (strcmp(matches[k], w) == 0) { exists = 1; break; }
                    }
                    if (!exists) strncpy(matches[match_count++], w, 255);
                }
                w = strtok(NULL, " ");
            }
            if (match_count > 0) used_custom_completions = 1;
        }
    }

    if (!used_custom_completions) {
    DIR *d = opendir(dir_to_open);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && match_count < 128) {
            if (strncmp(ent->d_name, match_prefix, strlen(match_prefix)) == 0) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

                char full_name[512];
                if (last_slash) {
                    snprintf(full_name, sizeof(full_name), "%.*s%s", (int)(last_slash - word + 1), word, ent->d_name);
                } else {
                    snprintf(full_name, sizeof(full_name), "%.*s%s", path_start, word, ent->d_name);
                }

                int exists = 0;
                for (int i = 0; i < match_count; i++) {
                    if (strcmp(matches[i], full_name) == 0) { exists = 1; break; }
                }
                if (!exists) {
                    char check_path[1024];
                    snprintf(check_path, sizeof(check_path), "%s/%s", dir_to_open, ent->d_name);
                    struct stat st;
                    if (stat(check_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                        strcat(full_name, "/");
                    }
                    strncpy(matches[match_count++], full_name, 255);
                }
            }
        }
        closedir(d);
    }
    }

    if (match_count == 1) {
        char *completion = matches[0];
        int comp_len = strlen(completion);

        memmove(&buffer[word_start + comp_len], &buffer[*cursor], *len - *cursor + 1);
        memcpy(&buffer[word_start], completion, comp_len);

        *len = *len - old_word_len + comp_len;
        *cursor = word_start + comp_len;

        if (buffer[*cursor - 1] != '/') {
            if (buffer[*cursor] != ' ') {
                memmove(&buffer[*cursor + 1], &buffer[*cursor], *len - *cursor + 1);
                buffer[*cursor] = ' ';
                (*len)++;
            }
            (*cursor)++;
        }

        refresh_line(buffer, *len, *cursor, prompt_len);

    } else if (match_count > 1) {
        char common_prefix[256];
        strcpy(common_prefix, matches[0]);
        for (int i = 1; i < match_count; i++) {
            int j = 0;
            while (common_prefix[j] && matches[i][j] && common_prefix[j] == matches[i][j]) j++;
            common_prefix[j] = '\0';
        }

        if (strlen(common_prefix) > strlen(word)) {
            int comp_len = strlen(common_prefix);

            memmove(&buffer[word_start + comp_len], &buffer[*cursor], *len - *cursor + 1);
            memcpy(&buffer[word_start], common_prefix, comp_len);

            *len = *len - old_word_len + comp_len;
            *cursor = word_start + comp_len;

            refresh_line(buffer, *len, *cursor, prompt_len);
        } else {
            write(1, "\r\n", 2);
            for (int i = 0; i < match_count; i++) {
                write(1, matches[i], strlen(matches[i]));
                write(1, "  ", 2);
            }
            write(1, "\r\n", 2);
            write(1, "\033[?25h", 6);
            
            old_cursor_rows = 0;
            refresh_line(buffer, *len, *cursor, prompt_len);
        }
    }
}

static int reverse_search(char *buffer, int prompt_len);
static void load_history_from_file(void);

int read_line_custom(char *buffer, struct termios *orig, int prompt_len) {
    int len = 0;
    int cursor = 0;
    load_history_from_file();
    static int hist_pos = 0;
    hist_pos = history_count;
    buffer[0] = '\0';

    enable_raw_mode(orig);
    old_cursor_rows = 0;

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) {
            disable_raw_mode(orig);
            return -1;
        }

        unsigned char uc = (unsigned char)c;

        if (uc == 3) { // Ctrl+C
            write(1, "^C\r\n", 4);
            buffer[0] = '\0';
            len = 0;
            cursor = 0;
            old_cursor_rows = 0;
            prompt_len = print_prompt();
            continue;
        }

        if (uc == 4) { // Ctrl+D (EOF)
            // Игнорируем нажатие, если в буфере есть текст
            if (len == 0) {
                disable_raw_mode(orig);
                return -1;
            }
            continue;
        }

        if (uc == 12) { // Ctrl+L (Очистка экрана)
            write(1, "\033[H\033[J", 7); // Очищаем экран и сдвигаем курсор в левый верхний угол
            old_cursor_rows = 0;
            prompt_len = print_prompt();
            refresh_line(buffer, len, cursor, prompt_len);
            continue;
        }

        if (uc == '\n' || uc == '\r') {
            write(1, "\r\n", 2);
            buffer[len] = '\0';
            break;
        }

        if (uc == '\t' || uc == 9) {
            do_tab_completion(buffer, &len, &cursor, prompt_len);
            continue;
        }

        if (uc == 18) { // Ctrl+R — reverse search
            int rlen = reverse_search(buffer, prompt_len);
            if (rlen >= 0) {
                len = rlen;
                cursor = len;
            }
            write(1, "\033[2K\033[A\033[2K\r", 12);
            prompt_len = print_prompt();
            write(1, buffer, len);
            cursor = len;
            continue;
        }

        if (uc == 1) { // Ctrl+A
            cursor = 0;
            refresh_line(buffer, len, cursor, prompt_len);
            continue;
        }
        
        if (uc == 5) { // Ctrl+E
            cursor = len;
            refresh_line(buffer, len, cursor, prompt_len);
            continue;
        }

        if (uc == 8 || uc == 23) { // Backspace / Ctrl+W
            int target = find_prev_word_start(buffer, cursor);
            int bytes_del = cursor - target;
            if (bytes_del > 0) {
                memmove(&buffer[target], &buffer[cursor], len - cursor + 1);
                len -= bytes_del;
                cursor = target;
                refresh_line(buffer, len, cursor, prompt_len);
            }
            continue;
        }

        if (uc == 127 || uc == '\b') {
            if (cursor > 0) {
                int prev = prev_u8_char(buffer, cursor);
                int bytes_to_delete = cursor - prev;
                memmove(&buffer[prev], &buffer[cursor], len - cursor + 1);
                cursor = prev;
                len -= bytes_to_delete;
                refresh_line(buffer, len, cursor, prompt_len);
            }
            continue;
        }

        if (uc == '\033') {
            char seq[16];
            int s_len = 0;
            char next;

            if (read(STDIN_FILENO, &next, 1) > 0) {
                seq[s_len++] = next;
                if (next == '[') {
                    while (s_len < (int)sizeof(seq) - 1) {
                        if (read(STDIN_FILENO, &next, 1) <= 0) break;
                        seq[s_len++] = next;
                        if ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') || next == '~') {
                            break;
                        }
                    }
                }
                seq[s_len] = '\0';
                char *body = &seq[1];
                int body_len = strlen(body);
                char last_char = body_len > 0 ? body[body_len - 1] : '\0';

                if (strcmp(body, "1;5D") == 0 || strstr(body, ";5D")) {
                    cursor = find_prev_word_start(buffer, cursor);
                    refresh_line(buffer, len, cursor, prompt_len);
                }
                else if (strcmp(body, "1;5C") == 0 || strstr(body, ";5C")) {
                    cursor = find_next_word_end(buffer, len, cursor);
                    refresh_line(buffer, len, cursor, prompt_len);
                }
                else if (strstr(body, "3;5")) {
                    int target = find_next_word_start(buffer, len, cursor);
                    int bytes_del = target - cursor;
                    if (bytes_del > 0) {
                        memmove(&buffer[cursor], &buffer[target], len - target + 1);
                        len -= bytes_del;
                        refresh_line(buffer, len, cursor, prompt_len);
                    }
                }
                else if (last_char == 'A') {
                    if (hist_pos > 0) {
                        hist_pos--;
                        strcpy(buffer, history[hist_pos]);
                        len = strlen(buffer);
                        cursor = len;
                        refresh_line(buffer, len, cursor, prompt_len);
                    }
                }
                else if (last_char == 'B') {
                    if (hist_pos < history_count) {
                        hist_pos++;
                        if (hist_pos < history_count) {
                            strcpy(buffer, history[hist_pos]);
                        } else {
                            buffer[0] = '\0';
                        }
                        len = strlen(buffer);
                        cursor = len;
                        refresh_line(buffer, len, cursor, prompt_len);
                    }
                }
                else if (last_char == 'C') {
                    if (cursor < len) {
                        cursor = next_u8_char(buffer, cursor, len);
                        refresh_line(buffer, len, cursor, prompt_len);
                    }
                }
                else if (last_char == 'D') {
                    if (cursor > 0) {
                        cursor = prev_u8_char(buffer, cursor);
                        refresh_line(buffer, len, cursor, prompt_len);
                    }
                }
                else if (last_char == 'H' || strstr(body, "1~") || strstr(body, "7~")) {
                    cursor = 0;
                    refresh_line(buffer, len, cursor, prompt_len);
                }
                else if (last_char == 'F' || strstr(body, "4~") || strstr(body, "8~")) {
                    cursor = len;
                    refresh_line(buffer, len, cursor, prompt_len);
                }
                else if (strncmp(body, "3", 1) == 0) {
                    if (cursor < len) {
                        int next = next_u8_char(buffer, cursor, len);
                        int bytes_to_delete = next - cursor;
                        memmove(&buffer[cursor], &buffer[next], len - next + 1);
                        len -= bytes_to_delete;
                        refresh_line(buffer, len, cursor, prompt_len);
                    }
                }
            }
            continue;
        }

        char u8_buf[4];
        int u8_len = 1;
        u8_buf[0] = c;

        if ((uc & 0xE0) == 0xC0) u8_len = 2;
        else if ((uc & 0xF0) == 0xE0) u8_len = 3;
        else if ((uc & 0xF8) == 0xF0) u8_len = 4;

        for (int i = 1; i < u8_len; i++) {
            if (read(STDIN_FILENO, &u8_buf[i], 1) <= 0) break;
        }

        if (len + u8_len < MAX_LINE - 1) {
            memmove(&buffer[cursor + u8_len], &buffer[cursor], len - cursor + 1);
            memcpy(&buffer[cursor], u8_buf, u8_len);
            len += u8_len;
            cursor += u8_len;
            refresh_line(buffer, len, cursor, prompt_len);
        }
    }

    disable_raw_mode(orig);
    return len;
}

static char history_path[512];
static int history_loaded = 0;

static void init_history_path(void) {
    char *home = getenv("HOME");
    if (home) {
        snprintf(history_path, sizeof(history_path), "%s/.nsh_history", home);
    }
}

static void load_history_from_file(void) {
    if (history_loaded) return;
    history_loaded = 1;
    init_history_path();
    if (history_path[0] == '\0') return;
    FILE *f = fopen(history_path, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') continue;
        if (history_count < MAX_HISTORY) {
            strncpy(history[history_count++], line, MAX_LINE - 1);
        } else {
            for (int i = 1; i < MAX_HISTORY; i++)
                strcpy(history[i - 1], history[i]);
            strncpy(history[MAX_HISTORY - 1], line, MAX_LINE - 1);
        }
    }
    fclose(f);
}

void save_history(void) {
    init_history_path();
    if (history_path[0] == '\0') return;

    FILE *f = fopen(history_path, "a");
    if (!f) return;
    fprintf(f, "%s\n", history[history_count - 1]);
    fclose(f);

    int total = 0;
    f = fopen(history_path, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) total++;
    fclose(f);

    if (total <= MAX_HISTORY) return;

    int skip = total - MAX_HISTORY;
    f = fopen(history_path, "r");
    if (!f) return;
    for (int i = 0; i < skip; i++) fgets(line, sizeof(line), f);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", history_path);
    FILE *tmp = fopen(tmp_path, "w");
    if (!tmp) { fclose(f); return; }
    while (fgets(line, sizeof(line), f)) fputs(line, tmp);
    fclose(tmp);
    fclose(f);

    rename(tmp_path, history_path);
}

static int reverse_search(char *buffer, int prompt_len) {
    init_history_path();
    char search_buf[MAX_LINE] = {0};
    int search_pos = 0;
    char found_line[MAX_LINE] = {0};
    int found = 0;
    int orig_len = strlen(buffer);
    char orig_buf[MAX_LINE];
    strcpy(orig_buf, buffer);

    write(1, "\r\n(reverse-i-search)`': ", 23);

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) {
            strcpy(buffer, orig_buf);
            return orig_len;
        }

        unsigned char uc = (unsigned char)c;

        if (uc == '\n' || uc == '\r') {
            if (found) {
                strcpy(buffer, found_line);
                return strlen(buffer);
            }
            strcpy(buffer, orig_buf);
            return orig_len;
        }

        if (uc == 3) {
            strcpy(buffer, orig_buf);
            return orig_len;
        }

        if (uc == 8 || uc == 127) {
            if (search_pos > 0) {
                search_pos--;
                search_buf[search_pos] = '\0';
            }
        } else if (uc == 27) {
            break;
        } else {
            if (search_pos < MAX_LINE - 2) {
                search_buf[search_pos++] = uc;
                search_buf[search_pos] = '\0';
            }
        }

        found = 0;
        found_line[0] = '\0';
        if (search_pos > 0 && history_path[0]) {
            FILE *f = fopen(history_path, "r");
            if (f) {
                char line[MAX_LINE];
                while (fgets(line, sizeof(line), f)) {
                    line[strcspn(line, "\n")] = '\0';
                    if (strstr(line, search_buf)) {
                        strcpy(found_line, line);
                        found = 1;
                    }
                }
                fclose(f);
            }
        }

        char prompt_line[256];
        int prompt_len2 = snprintf(prompt_line, sizeof(prompt_line),
            "\033[2K\r(reverse-i-search)`%s': ", search_buf);
        write(1, "\033[2K\r", 5);
        write(1, prompt_line, prompt_len2);
        if (found) {
            write(1, found_line, strlen(found_line));
        }
    }

    strcpy(buffer, orig_buf);
    return orig_len;
}
