#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <termios.h>
#include <errno.h>
#include <signal.h>

#include "config.h"
#include "term.h"
#include "prompt.h"

#define MAX_ARGS 64
#define ARG_SIZE 512

static void expand_tilde(const char *arg, char *out, size_t out_size) {
    if (arg[0] == '~' && (arg[1] == '/' || arg[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) {
            snprintf(out, out_size, "%s%s", home, arg + 1);
            return;
        }
    }
    strncpy(out, arg, out_size - 1);
    out[out_size - 1] = '\0';
}

static int parse_tokens(char *cmdline, char *args[], char arg_storage[MAX_ARGS][ARG_SIZE]) {
    int count = 0;
    char *p = cmdline;

    while (*p && count < MAX_ARGS - 1) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char buf[ARG_SIZE];
        int b_idx = 0;
        int in_squote = 0;
        int in_dquote = 0;

        while (*p) {
            if (*p == '\'' && !in_dquote) {
                in_squote = !in_squote;
                p++;
                continue;
            }
            if (*p == '"' && !in_squote) {
                in_dquote = !in_dquote;
                p++;
                continue;
            }
            if (isspace((unsigned char)*p) && !in_squote && !in_dquote) {
                break;
            }
            if (b_idx < ARG_SIZE - 1) {
                buf[b_idx++] = *p;
            }
            p++;
        }
        buf[b_idx] = '\0';

        expand_tilde(buf, arg_storage[count], sizeof(arg_storage[count]));
        args[count] = arg_storage[count];
        count++;
    }
    args[count] = NULL;
    return count;
}

static void parse_and_execute(char *cmdline) {
    if (cmdline == NULL || cmdline[0] == '\0') return;

    expand_alias(cmdline, MAX_LINE);

    char *args[MAX_ARGS];
    static char arg_storage[MAX_ARGS][ARG_SIZE];
    int arg_count = parse_tokens(cmdline, args, arg_storage);

    if (arg_count == 0) return;

    if (strcmp(args[0], "exit") == 0) {
        exit(0);
    }

    if (strcmp(args[0], "cd") == 0) {
        const char *dir = args[1];
        if (!dir) {
            dir = getenv("HOME");
        }
        if (dir && chdir(dir) != 0) {
            fprintf(stderr, "nsh: cd: %s: %s\n", dir, strerror(errno));
        }
        return;
    }

    if (is_function(args[0])) {
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            char *home = getenv("HOME");
            char rc_file[512] = "";
            if (home) {
                snprintf(rc_file, sizeof(rc_file), "%s/.nshrc", home);
            }

            char source_cmd[1024];
            snprintf(source_cmd, sizeof(source_cmd), "source %s 2>/dev/null; \"$@\"", rc_file);

            char *exec_args[MAX_ARGS + 5];
            exec_args[0] = "bash";
            exec_args[1] = "-c";
            exec_args[2] = source_cmd;
            exec_args[3] = "bash";

            for (int i = 0; i < arg_count; i++) {
                exec_args[4 + i] = args[i];
            }
            exec_args[4 + arg_count] = NULL;

            execvp("bash", exec_args);
            fprintf(stderr, "nsh: execvp bash: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        }
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        execvp(args[0], args);

        if (errno == ENOENT) {
            fprintf(stderr, "nsh: %s: command not found\n", args[0]);
            exit(127);
        } else if (errno == EACCES) {
            struct stat st;
            if (stat(args[0], &st) == 0 && S_ISDIR(st.st_mode)) {
                fprintf(stderr, "nsh: %s: Is a directory\n", args[0]);
            } else {
                fprintf(stderr, "nsh: %s: Permission denied\n", args[0]);
            }
            exit(126);
        } else {
            fprintf(stderr, "nsh: %s: %s\n", args[0], strerror(errno));
            exit(EXIT_FAILURE);
        }
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

int main(int argc, char *argv[]) {
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        execl("/bin/sh", "sh", "-c", argv[2], (char *)NULL);
        return 1;
    }

    char *home = getenv("HOME");
    if (home) {
        char path[512];
        snprintf(path, sizeof(path), "%s/.nshrc", home);
        load_bash_config(path);
    }

    signal(SIGINT, SIG_IGN);

    struct termios orig;
    char buffer[MAX_LINE];

    while (1) {
        write(1, "\033[?25h", 6);
        int prompt_len = print_prompt();
        int len = read_line_custom(buffer, &orig, prompt_len);
        
        if (len < 0) {
            write(1, "\n", 1);
            break;
        }

        if (len > 0) {
            if (history_count < MAX_HISTORY) {
                strncpy(history[history_count++], buffer, MAX_LINE - 1);
            } else {
                for (int i = 1; i < MAX_HISTORY; i++) {
                    strcpy(history[i - 1], history[i]);
                }
                strncpy(history[MAX_HISTORY - 1], buffer, MAX_LINE - 1);
            }

            parse_and_execute(buffer);
        }
    }

    return 0;
}
