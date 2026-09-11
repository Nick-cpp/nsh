#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <glob.h>

#include "config.h"
#include "term.h"
#include "prompt.h"

#define MAX_ARGS 64
#define ARG_SIZE 1024

static int last_exit_status = 0;
static int redir_in_fd = -1;
static int redir_out_fd = -1;
static int loop_break_flag = 0;
static int loop_continue_flag = 0;

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

static char *run_command_capture(const char *cmd) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return NULL; }

    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    char buf[4096];
    size_t total = 0;
    ssize_t n;
    while ((n = read(pipefd[0], buf + total, sizeof(buf) - total - 1)) > 0) {
        total += n;
        if (total >= sizeof(buf) - 1) break;
    }
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    if (total > 0) {
        while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
            total--;
        char *result = malloc(total + 1);
        if (result) {
            memcpy(result, buf, total);
            result[total] = '\0';
            return result;
        }
    }
    return NULL;
}

static void expand_cmd_subst(char *line, size_t max_len) {
    char result[ARG_SIZE * 4];
    int r_idx = 0;
    int i = 0;
    int len = strlen(line);

    while (i < len && r_idx < (int)sizeof(result) - 1) {
        if (line[i] == '$' && i + 1 < len && line[i + 1] == '(' && i + 2 < len && line[i + 2] == '(') {
            result[r_idx++] = line[i++];
            continue;
        }
        if (line[i] == '$' && i + 1 < len && line[i + 1] == '(') {
            int depth = 1;
            int start = i + 2;
            int j = start;
            while (j < len && depth > 0) {
                if (line[j] == '(') depth++;
                else if (line[j] == ')') depth--;
                j++;
            }
            if (depth == 0) {
                char cmd[512];
                int cmd_len = j - start - 1;
                if (cmd_len > 0 && cmd_len < (int)sizeof(cmd) - 1) {
                    memcpy(cmd, line + start, cmd_len);
                    cmd[cmd_len] = '\0';
                    char *output = run_command_capture(cmd);
                    if (output) {
                        for (int k = 0; output[k] && r_idx < (int)sizeof(result) - 1; k++) {
                            if (output[k] != '\n' && output[k] != '\r')
                                result[r_idx++] = output[k];
                            else if (output[k] == '\n' && r_idx < (int)sizeof(result) - 1)
                                result[r_idx++] = ' ';
                        }
                        free(output);
                    }
                    i = j;
                    continue;
                }
            }
        }
        result[r_idx++] = line[i];
        i++;
    }
    result[r_idx] = '\0';
    strncpy(line, result, max_len - 1);
    line[max_len - 1] = '\0';
}

static void expand_variable(const char *arg, char *out, size_t out_size) {
    int o_idx = 0;
    int i = 0;

    if (arg[0] == '~' && (arg[1] == '/' || arg[1] == '\0' || arg[1] == ' ')) {
        const char *home = getenv("HOME");
        if (home) {
            for (int k = 0; home[k] && o_idx < (int)out_size - 1; k++)
                out[o_idx++] = home[k];
        }
        i = 1;
    }

    while (arg[i] && o_idx < (int)out_size - 1) {
        if (arg[i] == '$' && arg[i + 1] == '(' && arg[i + 2] == '(') {
            int depth = 1;
            int start = i + 3;
            int j = start;
            while (arg[j] && depth > 0) {
                if (arg[j] == '(') depth++;
                else if (arg[j] == ')') {
                    depth--;
                    if (depth == 0 && arg[j + 1] == ')') break;
                }
                j++;
            }
            if (depth == 0) {
                char expr[512];
                int elen = j - start;
                if (elen > 0 && elen < (int)sizeof(expr) - 1) {
                    memcpy(expr, arg + start, elen);
                    expr[elen] = '\0';

                    char expanded_expr[1024];
                    expand_variable(expr, expanded_expr, sizeof(expanded_expr));

                    char calc_cmd[1200];
                    snprintf(calc_cmd, sizeof(calc_cmd), "echo $((%s))", expanded_expr);
                    char *result = run_command_capture(calc_cmd);
                    if (result) {
                        for (int k = 0; result[k] && o_idx < (int)out_size - 1; k++)
                            out[o_idx++] = result[k];
                        free(result);
                    }
                    i = j + 2;
                    continue;
                }
            }
        }
        if (arg[i] == '$') {
            i++;
            if (arg[i] == '?') {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", last_exit_status);
                for (int k = 0; buf[k] && o_idx < (int)out_size - 1; k++)
                    out[o_idx++] = buf[k];
                i++;
            } else if (arg[i] == '$') {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", getpid());
                for (int k = 0; buf[k] && o_idx < (int)out_size - 1; k++)
                    out[o_idx++] = buf[k];
                i++;
            } else if (arg[i] == '#') {
                char buf[16] = "0";
                for (int k = 0; buf[k] && o_idx < (int)out_size - 1; k++)
                    out[o_idx++] = buf[k];
                i++;
            } else if (arg[i] == '{') {
                i++;
                char varname[256];
                int v_idx = 0;
                while (arg[i] && arg[i] != '}' && arg[i] != ':' && v_idx < (int)sizeof(varname) - 1)
                    varname[v_idx++] = arg[i++];
                varname[v_idx] = '\0';

                if (arg[i] == ':') {
                    i++;
                    char op = arg[i];
                    if (op == '-' || op == '=' || op == '+' || op == '?') {
                        i++;
                        char default_val[512];
                        int d_idx = 0;
                        int d_depth = 0;
                        while (arg[i] && (arg[i] != '}' || d_depth > 0)) {
                            if (arg[i] == '{') d_depth++;
                            else if (arg[i] == '}') d_depth--;
                            if (d_depth >= 0 && d_idx < (int)sizeof(default_val) - 1)
                                default_val[d_idx++] = arg[i];
                            i++;
                        }
                        default_val[d_idx] = '\0';

                        if (arg[i] == '}') i++;

                        const char *val = getenv(varname);
                        int val_set = (val != NULL);
                        int val_nonempty = (val && val[0] != '\0');

                        const char *result = NULL;
                        if (op == '-') {
                            result = (val_set && val_nonempty) ? val : default_val;
                        } else if (op == '=') {
                            if (!val_set || !val_nonempty) {
                                setenv(varname, default_val, 1);
                                result = getenv(varname);
                            } else {
                                result = val;
                            }
                        } else if (op == '+') {
                            result = (val_set && val_nonempty) ? default_val : NULL;
                        } else if (op == '?') {
                            if (!val_set || !val_nonempty) {
                                fprintf(stderr, "nsh: %s: %s\n", varname, default_val);
                            }
                            result = val;
                        }

                        if (result) {
                            for (int k = 0; result[k] && o_idx < (int)out_size - 1; k++)
                                out[o_idx++] = result[k];
                        }
                    } else {
                        /* Handle ${VAR:offset} or ${VAR:offset:length} - skip for now */
                        while (arg[i] && arg[i] != '}') i++;
                        if (arg[i] == '}') i++;
                        const char *val = getenv(varname);
                        if (val) {
                            for (int k = 0; val[k] && o_idx < (int)out_size - 1; k++)
                                out[o_idx++] = val[k];
                        }
                    }
                } else if (arg[i] == '}') {
                    i++;
                    const char *val = getenv(varname);
                    if (val) {
                        for (int k = 0; val[k] && o_idx < (int)out_size - 1; k++)
                            out[o_idx++] = val[k];
                    }
                }
            } else if (isalpha((unsigned char)arg[i]) || arg[i] == '_') {
                char varname[256];
                int v_idx = 0;
                while (arg[i] && (isalnum((unsigned char)arg[i]) || arg[i] == '_')
                       && v_idx < (int)sizeof(varname) - 1)
                    varname[v_idx++] = arg[i++];
                varname[v_idx] = '\0';
                const char *val = getenv(varname);
                if (val) {
                    for (int k = 0; val[k] && o_idx < (int)out_size - 1; k++)
                        out[o_idx++] = val[k];
                }
            } else {
                if (o_idx < (int)out_size - 1) out[o_idx++] = '$';
            }
        } else {
            out[o_idx++] = arg[i];
            i++;
        }
    }
    out[o_idx] = '\0';
}

static int expand_glob(const char *pattern, char matches[][ARG_SIZE], int max_matches) {
    glob_t globbuf;
    int count = 0;

    if (glob(pattern, 0, NULL, &globbuf) == 0) {
        for (size_t i = 0; i < globbuf.gl_pathc && count < max_matches; i++) {
            strncpy(matches[count], globbuf.gl_pathv[i], ARG_SIZE - 1);
            matches[count][ARG_SIZE - 1] = '\0';
            count++;
        }
        globfree(&globbuf);
    }

    return count;
}

static void expand_token(const char *token, char expanded[][ARG_SIZE], int *count) {
    char var_expanded[ARG_SIZE];
    expand_variable(token, var_expanded, sizeof(var_expanded));

    if (strchr(var_expanded, '*') || strchr(var_expanded, '?') || strchr(var_expanded, '[')) {
        int n = expand_glob(var_expanded, expanded + *count, MAX_ARGS - *count);
        if (n > 0) {
            *count += n;
            return;
        }
    }

    strncpy(expanded[*count], var_expanded, ARG_SIZE - 1);
    expanded[*count][ARG_SIZE - 1] = '\0';
    (*count)++;
}

static void free_args(char **args) {
    for (int i = 0; args[i]; i++) {
        free(args[i]);
        args[i] = NULL;
    }
}

static void parse_and_execute(char *cmdline);

static int tokenize(char *cmdline, char *args[]) {
    int count = 0;
    char *p = cmdline;

    while (*p && count < MAX_ARGS - 1) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char buf[ARG_SIZE];
        int b_idx = 0;
        int in_squote = 0;
        int in_dquote = 0;
        int tilde_quoted = 0;

        while (*p) {
            if (*p == '\\' && !in_squote) {
                p++;
                if (*p && b_idx < ARG_SIZE - 1) buf[b_idx++] = *p++;
                continue;
            }
            if (*p == '\'' && !in_dquote) {
                in_squote = !in_squote;
                if (b_idx == 0) tilde_quoted = 1;
                p++;
                continue;
            }
            if (*p == '"' && !in_squote) {
                in_dquote = !in_dquote;
                if (b_idx == 0) tilde_quoted = 1;
                p++;
                continue;
            }
            if (*p == '$' && p[1] == '(' && p[2] == '(' && !in_squote && !in_dquote) {
                int depth = 1;
                int start = p - cmdline;
                p += 3;
                while (*p && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') {
                        depth--;
                        if (depth == 0 && p[1] == ')') { p += 2; break; }
                    }
                    p++;
                }
                int len = (int)(p - cmdline - start);
                if (b_idx + len < ARG_SIZE - 1) {
                    memcpy(buf + b_idx, cmdline + start, len);
                    b_idx += len;
                }
                continue;
            }
            if (isspace((unsigned char)*p) && !in_squote && !in_dquote) break;
            if (b_idx < ARG_SIZE - 1) buf[b_idx++] = *p;
            p++;
        }
        buf[b_idx] = '\0';

        if (b_idx == 0) continue;

        if (buf[0] == '~' && !tilde_quoted) {
            const char *home = getenv("HOME");
            if (home) {
                char expanded_tilde[ARG_SIZE];
                snprintf(expanded_tilde, sizeof(expanded_tilde), "%s%s", home, buf + 1);
                strncpy(buf, expanded_tilde, ARG_SIZE - 1);
                buf[ARG_SIZE - 1] = '\0';
            }
        }

        char expanded[MAX_ARGS][ARG_SIZE];
        int exp_count = 0;
        expand_token(buf, expanded, &exp_count);

        for (int e = 0; e < exp_count && count < MAX_ARGS - 1; e++) {
            args[count] = malloc(strlen(expanded[e]) + 1);
            strcpy(args[count], expanded[e]);
            count++;
        }
    }
    args[count] = NULL;
    return count;
}

static int is_number(const char *s) {
    if (!s || !*s) return 0;
    for (int i = 0; s[i]; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

static int run_test(char **args, int argc) {
    if (argc < 2) return 1;

    int negate = 0;
    int ai = 1;

    if (strcmp(args[ai], "!") == 0 && argc > 2) {
        negate = 1;
        ai++;
    }

    if (strcmp(args[ai], "[") == 0) ai++;
    if (ai >= argc) return 1;
    if (strcmp(args[argc - 1], "]") == 0) argc--;

    int remaining = argc - ai;

    if (remaining == 1) {
        int result = (strlen(args[ai]) > 0);
        return (negate ? !result : result) ? 0 : 1;
    }

    if (remaining == 2) {
        const char *op = args[ai];
        const char *val = args[ai + 1];
        int result = 1;
        if (strcmp(op, "-f") == 0) { struct stat st; result = (stat(val, &st) == 0 && S_ISREG(st.st_mode)); }
        else if (strcmp(op, "-d") == 0) { struct stat st; result = (stat(val, &st) == 0 && S_ISDIR(st.st_mode)); }
        else if (strcmp(op, "-e") == 0) { struct stat st; result = (stat(val, &st) == 0); }
        else if (strcmp(op, "-z") == 0) { result = (strlen(val) == 0); }
        else if (strcmp(op, "-n") == 0) { result = (strlen(val) > 0); }
        else if (strcmp(op, "-r") == 0 || strcmp(op, "-w") == 0 || strcmp(op, "-x") == 0) {
            struct stat st; result = (stat(val, &st) == 0);
        }
        else { result = (strlen(op) > 0); }
        return (negate ? !result : result) ? 0 : 1;
    }

    if (remaining == 3) {
        const char *left = args[ai];
        const char *op = args[ai + 1];
        const char *right = args[ai + 2];
        int result = 1;

        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) result = (strcmp(left, right) == 0);
        else if (strcmp(op, "!=") == 0) result = (strcmp(left, right) != 0);
        else if (strcmp(op, "-eq") == 0) result = (atoi(left) == atoi(right));
        else if (strcmp(op, "-ne") == 0) result = (atoi(left) != atoi(right));
        else if (strcmp(op, "-lt") == 0) result = (atoi(left) < atoi(right));
        else if (strcmp(op, "-le") == 0) result = (atoi(left) <= atoi(right));
        else if (strcmp(op, "-gt") == 0) result = (atoi(left) > atoi(right));
        else if (strcmp(op, "-ge") == 0) result = (atoi(left) >= atoi(right));

        return (negate ? !result : result) ? 0 : 1;
    }

    return 1;
}

static int exec_simple(char **args, int argc);

static int handle_if(char *body) {
    char *then_kw = NULL;
    int depth = 0;
    char *p = body;
    int in_sq = 0, in_dq = 0;

    while (*p) {
        if (*p == '\\' && !in_sq) { p++; if (*p) p++; continue; }
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
        if (*p == '"' && !in_sq) { in_dq = !in_dq; p++; continue; }
        if (in_sq || in_dq) { p++; continue; }

        if (strncmp(p, "if", 2) == 0 && (p[2] == ' ' || p[2] == '\t' || p[2] == '\n' || p[2] == ';')) depth++;
        if (depth == 0 && strncmp(p, "then", 4) == 0 && (p[4] == ' ' || p[4] == '\t' || p[4] == '\n' || p[4] == ';' || p[4] == '\0')) {
            then_kw = p;
            break;
        }
        if (strncmp(p, "fi", 2) == 0 && (p[2] == ' ' || p[2] == '\t' || p[2] == '\n' || p[2] == '\0' || p[2] == ';')) {
            if (depth > 0) depth--;
        }
        p++;
    }

    if (!then_kw) {
        fprintf(stderr, "nsh: syntax error: missing then\n");
        return -1;
    }

    char *cond = body;
    *then_kw = '\0';

    char *then_body = then_kw + 4;
    while (*then_body == ' ' || *then_body == '\t' || *then_body == '\n' || *then_body == ';') then_body++;

    char *else_kw = NULL;
    char *fi_kw = NULL;
    depth = 0;
    p = then_body;
    in_sq = 0;
    in_dq = 0;

    while (*p) {
        if (*p == '\\' && !in_sq) { p++; if (*p) p++; continue; }
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
        if (*p == '"' && !in_sq) { in_dq = !in_dq; p++; continue; }
        if (in_sq || in_dq) { p++; continue; }

        if (strncmp(p, "if", 2) == 0 && (p[2] == ' ' || p[2] == '\t' || p[2] == '\n' || p[2] == ';')) depth++;
        if (depth == 0 && !else_kw && strncmp(p, "else", 4) == 0 && (p[4] == ' ' || p[4] == '\t' || p[4] == '\n' || p[4] == ';' || p[4] == '\0')) {
            else_kw = p;
        }
        if (depth == 0 && strncmp(p, "fi", 2) == 0 && (p[2] == ' ' || p[2] == '\t' || p[2] == '\n' || p[2] == '\0' || p[2] == ';')) {
            fi_kw = p;
            break;
        }
        if (strncmp(p, "fi", 2) == 0 && (p[2] == ' ' || p[2] == '\t' || p[2] == '\n' || p[2] == '\0' || p[2] == ';'))
            depth--;
        p++;
    }

    if (!fi_kw) {
        fprintf(stderr, "nsh: syntax error: missing fi\n");
        return -1;
    }

    *fi_kw = '\0';

    parse_and_execute(cond);
    int result = last_exit_status;

    char *block;
    if (result == 0) {
        block = then_body;
        if (else_kw) *else_kw = '\0';
    } else if (else_kw) {
        block = else_kw + 4;
        while (*block == ' ' || *block == '\t' || *block == '\n' || *block == ';') block++;
    } else {
        return result;
    }

    parse_and_execute(block);
    return last_exit_status;
}

static int handle_for(char *body) {
    char *p = body;
    while (*p == ' ' || *p == '\t') p++;

    char name[256];
    int ni = 0;
    while (*p && !isspace((unsigned char)*p) && ni < (int)sizeof(name) - 1)
        name[ni++] = *p++;
    name[ni] = '\0';

    while (*p == ' ' || *p == '\t') p++;

    if (strncmp(p, "in", 2) != 0 || (p[2] != ' ' && p[2] != '\t' && p[2] != ';' && p[2] != '\n')) {
        fprintf(stderr, "nsh: syntax error: missing in\n");
        return -1;
    }
    p += 2;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';') p++;

    char *do_kw = NULL;
    char *search = p;
    while (*search) {
        if (strncmp(search, "do", 2) == 0 && (search[2] == ' ' || search[2] == '\t' || search[2] == ';' || search[2] == '\n')) {
            do_kw = search;
            break;
        }
        search++;
    }
    if (!do_kw) {
        fprintf(stderr, "nsh: syntax error: missing do\n");
        return -1;
    }

    char *items_str = p;
    *do_kw = '\0';

    {
        size_t ilen = strlen(items_str);
        while (ilen > 0 && (items_str[ilen - 1] == ' ' || items_str[ilen - 1] == '\t' ||
                            items_str[ilen - 1] == ';' || items_str[ilen - 1] == '\n'))
            items_str[--ilen] = '\0';
    }

    char *loop_body = do_kw + 2;
    while (*loop_body == ' ' || *loop_body == '\t' || *loop_body == ';' || *loop_body == '\n') loop_body++;

    char *done_kw = NULL;
    int depth = 0;
    char *dp = loop_body;
    int in_sq = 0, in_dq = 0;
    while (*dp) {
        if (*dp == '\\' && !in_sq) { dp++; if (*dp) dp++; continue; }
        if (*dp == '\'' && !in_dq) { in_sq = !in_sq; dp++; continue; }
        if (*dp == '"' && !in_sq) { in_dq = !in_dq; dp++; continue; }
        if (in_sq || in_dq) { dp++; continue; }
        if (strncmp(dp, "for", 3) == 0 && (dp[3] == ' ' || dp[3] == '\t' || dp[3] == '\n')) depth++;
        if (depth == 0 && strncmp(dp, "done", 4) == 0 && (dp[4] == ' ' || dp[4] == '\t' || dp[4] == '\n' || dp[4] == '\0' || dp[4] == ';')) {
            done_kw = dp;
            break;
        }
        if (strncmp(dp, "done", 4) == 0 && (dp[4] == ' ' || dp[4] == '\t' || dp[4] == '\n' || dp[4] == '\0' || dp[4] == ';'))
            depth--;
        dp++;
    }

    if (!done_kw) {
        fprintf(stderr, "nsh: syntax error: missing done\n");
        return -1;
    }

    *done_kw = '\0';

    char *args[MAX_ARGS];
    int argc = tokenize(items_str, args);

    int result = 0;
    for (int i = 0; i < argc; i++) {
        setenv(name, args[i], 1);
        loop_break_flag = 0;
        loop_continue_flag = 0;
        parse_and_execute(loop_body);
        if (loop_break_flag) { loop_break_flag = 0; break; }
        if (loop_continue_flag) { loop_continue_flag = 0; continue; }
        result = last_exit_status;
    }

    free_args(args);
    return result;
}

static int handle_while(char *body, int negate) {
    char *do_kw = NULL;
    char *search = body;
    while (*search) {
        if (strncmp(search, "do", 2) == 0 && (search[2] == ' ' || search[2] == '\t' || search[2] == ';' || search[2] == '\n')) {
            do_kw = search;
            break;
        }
        search++;
    }
    if (!do_kw) {
        fprintf(stderr, "nsh: syntax error: missing do\n");
        return -1;
    }

    char *cond = body;
    *do_kw = '\0';

    char *loop_body = do_kw + 2;
    while (*loop_body == ' ' || *loop_body == '\t' || *loop_body == ';' || *loop_body == '\n') loop_body++;

    char *done_kw = NULL;
    int depth = 0;
    char *dp = loop_body;
    int in_sq = 0, in_dq = 0;
    while (*dp) {
        if (*dp == '\\' && !in_sq) { dp++; if (*dp) dp++; continue; }
        if (*dp == '\'' && !in_dq) { in_sq = !in_sq; dp++; continue; }
        if (*dp == '"' && !in_sq) { in_dq = !in_dq; dp++; continue; }
        if (in_sq || in_dq) { dp++; continue; }
        if (!negate && strncmp(dp, "while", 5) == 0 && (dp[5] == ' ' || dp[5] == '\t' || dp[5] == '\n')) depth++;
        if (negate && strncmp(dp, "until", 5) == 0 && (dp[5] == ' ' || dp[5] == '\t' || dp[5] == '\n')) depth++;
        if (depth == 0 && strncmp(dp, "done", 4) == 0 && (dp[4] == ' ' || dp[4] == '\t' || dp[4] == '\n' || dp[4] == '\0' || dp[4] == ';')) {
            done_kw = dp;
            break;
        }
        if (strncmp(dp, "done", 4) == 0 && (dp[4] == ' ' || dp[4] == '\t' || dp[4] == '\n' || dp[4] == '\0' || dp[4] == ';'))
            depth--;
        dp++;
    }

    if (!done_kw) {
        fprintf(stderr, "nsh: syntax error: missing done\n");
        return -1;
    }

    *done_kw = '\0';

    int result = 0;
    int iterations = 0;
    while (iterations < 1000000) {
        parse_and_execute(cond);
        int cond_result = last_exit_status;
        int should_run = negate ? (cond_result != 0) : (cond_result == 0);
        if (!should_run) break;
        loop_break_flag = 0;
        loop_continue_flag = 0;
        parse_and_execute(loop_body);
        if (loop_break_flag) { loop_break_flag = 0; break; }
        if (loop_continue_flag) { loop_continue_flag = 0; iterations++; continue; }
        result = last_exit_status;
        iterations++;
    }

    return result;
}

static int handle_case(char *body) {
    char *p = body;
    while (*p == ' ' || *p == '\t') p++;

    char word_raw[256];
    int wi = 0;
    while (*p && !isspace((unsigned char)*p) && wi < (int)sizeof(word_raw) - 1)
        word_raw[wi++] = *p++;
    word_raw[wi] = '\0';

    char word[256];
    expand_variable(word_raw, word, sizeof(word));

    if ((word[0] == '"' && word[strlen(word)-1] == '"') ||
        (word[0] == '\'' && word[strlen(word)-1] == '\'')) {
        int len = strlen(word);
        memmove(word, word + 1, len - 2);
        word[len - 2] = '\0';
    }

    while (*p == ' ' || *p == '\t') p++;

    if (strncmp(p, "in", 2) != 0 || (p[2] != ' ' && p[2] != '\t' && p[2] != '\n' && p[2] != '\0')) {
        fprintf(stderr, "nsh: syntax error: missing in\n");
        return -1;
    }
    p += 2;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;

    char *esac_kw = NULL;
    char *search = p;
    while (*search) {
        if (strncmp(search, "esac", 4) == 0 && (search[4] == ' ' || search[4] == '\t' || search[4] == '\n' || search[4] == '\0')) {
            esac_kw = search;
            break;
        }
        search++;
    }

    if (!esac_kw) {
        fprintf(stderr, "nsh: syntax error: missing esac\n");
        return -1;
    }

    *esac_kw = '\0';

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;

        char pattern[256];
        int pi = 0;
        while (*p && *p != ')' && pi < (int)sizeof(pattern) - 1)
            pattern[pi++] = *p++;
        pattern[pi] = '\0';

        while (pi > 0 && (pattern[pi-1] == ' ' || pattern[pi-1] == '\t'))
            pattern[--pi] = '\0';
        char *pp = pattern;
        while (*pp == ' ' || *pp == '\t') pp++;

        if (*p == ')') p++;
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;

        char *case_body_start = p;

        while (*p) {
            if (strncmp(p, ";;", 2) == 0) {
                *p = '\0';
                p += 2;
                break;
            }
            p++;
        }

        int matched = 0;
        if (strcmp(pp, "*") == 0) {
            matched = 1;
        } else {
            matched = 1;
            const char *wp = word;
            const char *ppat = pp;
            while (*wp && *ppat) {
                if (*ppat == '*') {
                    ppat++;
                    if (!*ppat) break;
                    while (*wp && *wp != *ppat) wp++;
                    if (!*wp) { matched = 0; break; }
                } else if (*ppat == '?' || *wp == *ppat) {
                    wp++;
                    ppat++;
                } else {
                    matched = 0;
                    break;
                }
            }
            if ((*wp || *ppat) && matched && *ppat != '*') matched = 0;
        }

        if (matched) {
            parse_and_execute(case_body_start);
            return last_exit_status;
        }
    }

    return 0;
}

static void execute_segment(char *cmd);

static void execute_pipe(char *cmd) {
    char *segs[MAX_ARGS];
    int seg_count = 0;
    char *p = cmd;

    while (*p && seg_count < MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char buf[ARG_SIZE];
        int b_idx = 0;
        int in_sq = 0, in_dq = 0;

        while (*p) {
            if (*p == '\\' && !in_sq) { buf[b_idx++] = *p++; if (*p) buf[b_idx++] = *p++; continue; }
            if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
            if (*p == '"' && !in_sq) { in_dq = !in_dq; p++; continue; }
            if (!in_sq && !in_dq && *p == '|') break;
            buf[b_idx++] = *p++;
        }
        buf[b_idx] = '\0';

        segs[seg_count] = malloc(strlen(buf) + 1);
        strcpy(segs[seg_count], buf);
        seg_count++;

        if (*p == '|') p++;
    }

    if (seg_count <= 1) {
        if (seg_count == 1) {
            char *args[MAX_ARGS];
            int argc = tokenize(segs[0], args);
            if (argc > 0) exec_simple(args, argc);
            free_args(args);
        }
        for (int i = 0; i < seg_count; i++) free(segs[i]);
        return;
    }

    int in_fd = -1;
    pid_t pids[MAX_ARGS];
    int pid_count = 0;

    for (int i = 0; i < seg_count; i++) {
        int pipefd[2];
        if (i < seg_count - 1) {
            if (pipe(pipefd) < 0) { perror("pipe"); break; }
        }

        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            if (in_fd != -1) { dup2(in_fd, STDIN_FILENO); close(in_fd); }
            if (i < seg_count - 1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            char *args[MAX_ARGS];
            int argc = tokenize(segs[i], args);
            if (argc > 0) {
                if (strcmp(args[0], "test") == 0 || strcmp(args[0], "[") == 0)
                    exit(run_test(args, argc));
                execvp(args[0], args);
                if (errno == ENOENT) { fprintf(stderr, "nsh: %s: command not found\n", args[0]); exit(127); }
                else if (errno == EACCES) { fprintf(stderr, "nsh: %s: Permission denied\n", args[0]); exit(126); }
                else { fprintf(stderr, "nsh: %s: %s\n", args[0], strerror(errno)); exit(EXIT_FAILURE); }
            }
            exit(0);
        } else if (pid > 0) {
            pids[pid_count++] = pid;
            if (in_fd != -1) close(in_fd);
            if (i < seg_count - 1) {
                close(pipefd[1]);
                in_fd = pipefd[0];
            }
        }
    }

    for (int i = 0; i < pid_count; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (i == pid_count - 1) {
            if (WIFEXITED(status)) last_exit_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) last_exit_status = 128 + WTERMSIG(status);
        }
    }

    for (int i = 0; i < seg_count; i++) free(segs[i]);
}

static int exec_simple(char **args, int argc) {
    if (argc == 0) return 0;

    if (strcmp(args[0], "exit") == 0) exit(0);

    if (strcmp(args[0], "cd") == 0) {
        const char *dir = args[1];
        if (!dir) dir = getenv("HOME");
        if (dir && chdir(dir) != 0) fprintf(stderr, "nsh: cd: %s: %s\n", dir, strerror(errno));
        return 0;
    }

    if (strcmp(args[0], "export") == 0) {
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(args[i], '=');
            if (eq) {
                *eq = '\0';
                char exp_val[1024];
                expand_variable(eq + 1, exp_val, sizeof(exp_val));
                setenv(args[i], exp_val, 1);
            } else {
                if (getenv(args[i]) == NULL) setenv(args[i], "", 1);
            }
        }
        return 0;
    }

    if (strcmp(args[0], "unset") == 0) {
        for (int i = 1; i < argc; i++)
            unsetenv(args[i]);
        return 0;
    }

    if (strcmp(args[0], "source") == 0 || strcmp(args[0], ".") == 0) {
        if (argc < 2) { fprintf(stderr, "nsh: %s: filename required\n", args[0]); return 1; }
        FILE *f = fopen(args[1], "r");
        if (!f) { fprintf(stderr, "nsh: %s: %s: %s\n", args[0], args[1], strerror(errno)); return 1; }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize <= 0) { fclose(f); return 0; }
        char *content = malloc(fsize + 1);
        if (!content) { fclose(f); return 1; }
        fread(content, 1, fsize, f);
        content[fsize] = '\0';
        fclose(f);
        parse_and_execute(content);
        free(content);
        return last_exit_status;
    }

    if (strcmp(args[0], "test") == 0 || strcmp(args[0], "[") == 0) {
        last_exit_status = run_test(args, argc);
        return last_exit_status;
    }

    if (strcmp(args[0], "complete") == 0) {
        if (argc >= 3 && strcmp(args[1], "-c") == 0) {
            const char *cmd_name = args[2];
            const char *words = "";
            for (int i = 3; i < argc - 1; i++) {
                if (strcmp(args[i], "-W") == 0) {
                    words = args[i + 1];
                    break;
                }
            }
            add_completion(cmd_name, words);
        }
        return 0;
    }

    if (strcmp(args[0], "break") == 0) { loop_break_flag = 1; return 0; }
    if (strcmp(args[0], "continue") == 0) { loop_continue_flag = 1; return 0; }
    if (strcmp(args[0], "true") == 0) { return 0; }
    if (strcmp(args[0], "false") == 0) { return 1; }
    if (strcmp(args[0], "hash") == 0) { return 0; }

    if (is_function(args[0])) {
        const char *body = get_function_body(args[0]);
        if (body && *body) {
            char func_body[8192];
            strncpy(func_body, body, sizeof(func_body) - 1);
            func_body[sizeof(func_body) - 1] = '\0';

            char *old_val[argc];
            for (int i = 1; i < argc; i++) {
                char varname[32];
                snprintf(varname, sizeof(varname), "%d", i);
                old_val[i] = getenv(varname);
                setenv(varname, args[i], 1);
            }
            setenv("@", "", 1);

            int old_loop_break = loop_break_flag;
            int old_loop_continue = loop_continue_flag;
            parse_and_execute(func_body);
            int ret = last_exit_status;
            loop_break_flag = old_loop_break;
            loop_continue_flag = old_loop_continue;

            for (int i = 1; i < argc; i++) {
                char varname[32];
                snprintf(varname, sizeof(varname), "%d", i);
                if (old_val[i]) setenv(varname, old_val[i], 1);
                else unsetenv(varname);
            }
            return ret;
        }
        return 0;
    }



    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        if (redir_in_fd >= 0) { dup2(redir_in_fd, STDIN_FILENO); close(redir_in_fd); }
        if (redir_out_fd >= 0) { dup2(redir_out_fd, STDOUT_FILENO); close(redir_out_fd); }
        execvp(args[0], args);
        if (errno == ENOENT) {
            fprintf(stderr, "nsh: %s: command not found\n", args[0]);
            exit(127);
        } else if (errno == EACCES) {
            struct stat st;
            if (stat(args[0], &st) == 0 && S_ISDIR(st.st_mode))
                fprintf(stderr, "nsh: %s: Is a directory\n", args[0]);
            else
                fprintf(stderr, "nsh: %s: Permission denied\n", args[0]);
            exit(126);
        } else {
            fprintf(stderr, "nsh: %s: %s\n", args[0], strerror(errno));
            exit(EXIT_FAILURE);
        }
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) last_exit_status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) last_exit_status = 128 + WTERMSIG(status);
    }
    return 0;
}

static void execute_segment(char *cmd) {
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (!*cmd) return;


    if (strncmp(cmd, "if ", 3) == 0 || strncmp(cmd, "if\t", 3) == 0 || strncmp(cmd, "if\n", 3) == 0 || strcmp(cmd, "if") == 0) {
        handle_if(cmd[2] ? cmd + 3 : cmd + 2);
        return;
    }
    if (strncmp(cmd, "for ", 4) == 0 || strncmp(cmd, "for\t", 4) == 0 || strncmp(cmd, "for\n", 4) == 0 || strcmp(cmd, "for") == 0) {
        handle_for(cmd[3] ? cmd + 4 : cmd + 3);
        return;
    }
    if (strncmp(cmd, "while ", 6) == 0 || strncmp(cmd, "while\t", 6) == 0 || strcmp(cmd, "while") == 0) {
        handle_while(cmd[5] ? cmd + 6 : cmd + 5, 0);
        return;
    }
    if (strncmp(cmd, "until ", 6) == 0 || strncmp(cmd, "until\t", 6) == 0 || strcmp(cmd, "until") == 0) {
        handle_while(cmd[5] ? cmd + 6 : cmd + 5, 1);
        return;
    }
    if (strncmp(cmd, "case ", 5) == 0 || strncmp(cmd, "case\t", 5) == 0 || strcmp(cmd, "case") == 0) {
        handle_case(cmd[4] ? cmd + 5 : cmd + 4);
        return;
    }

    {
        char *op_pos = NULL;
        int is_and = 1;
        int in_sq = 0, in_dq = 0;

        for (char *p = cmd; *p; p++) {
            if (*p == '\\' && !in_sq) { p++; continue; }
            if (*p == '\'' && !in_dq) { in_sq = !in_sq; continue; }
            if (*p == '"' && !in_sq) { in_dq = !in_dq; continue; }
            if (!in_sq && !in_dq) {
                if (p[0] == '&' && p[1] == '&' && p[2] == ' ') {
                    op_pos = p; is_and = 1; break;
                }
                if (p[0] == '|' && p[1] == '|' && p[2] == ' ') {
                    op_pos = p; is_and = 0; break;
                }
            }
        }

        if (op_pos) {
            *op_pos = '\0';
            execute_segment(cmd);
            if (loop_break_flag || loop_continue_flag) return;
            if (is_and) {
                if (last_exit_status == 0) execute_segment(op_pos + 3);
            } else {
                if (last_exit_status != 0) execute_segment(op_pos + 3);
            }
            return;
        }
    }

    if (strstr(cmd, " | ")) {
        execute_pipe(cmd);
        return;
    }

    redir_out_fd = -1;
    redir_in_fd = -1;
    int redir_out_append = 0;
    char redir_out_file[512] = {0}, redir_in_file[512] = {0};

    {
        char clean[ARG_SIZE];
        int c_idx = 0;
        char *p = cmd;
        int in_sq = 0, in_dq = 0;

        while (*p) {
            if (*p == '\\' && !in_sq) { clean[c_idx++] = *p++; if (*p) clean[c_idx++] = *p++; continue; }
            if (*p == '\'' && !in_dq) { in_sq = !in_sq; clean[c_idx++] = *p++; continue; }
            if (*p == '"' && !in_sq) { in_dq = !in_dq; clean[c_idx++] = *p++; continue; }
            if (!in_sq && !in_dq && *p == '>' && p[1] == '>') {
                p += 2;
                while (*p == ' ' || *p == '\t') p++;
                int f_idx = 0;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ';' && *p != '|') {
                    redir_out_file[f_idx++] = *p++;
                }
                redir_out_file[f_idx] = '\0';
                redir_out_append = 1;
                continue;
            }
            if (!in_sq && !in_dq && *p == '>') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                int f_idx = 0;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ';' && *p != '|') {
                    redir_out_file[f_idx++] = *p++;
                }
                redir_out_file[f_idx] = '\0';
                redir_out_append = 0;
                continue;
            }
            if (!in_sq && !in_dq && *p == '<') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                int f_idx = 0;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ';' && *p != '|') {
                    redir_in_file[f_idx++] = *p++;
                }
                redir_in_file[f_idx] = '\0';
                continue;
            }
            clean[c_idx++] = *p++;
        }
        clean[c_idx] = '\0';
        strcpy(cmd, clean);
    }

    char *env_save[MAX_ARGS][2];
    int env_count = 0;

    char *cmd_start = cmd;

    while (*cmd_start == ' ' || *cmd_start == '\t') cmd_start++;

    while (*cmd_start) {
        char *eq = NULL;
        int in_sq = 0, in_dq = 0;
        for (char *p = cmd_start; *p; p++) {
            if (*p == '\\' && !in_sq) { p++; continue; }
            if (*p == '\'' && !in_dq) { in_sq = !in_sq; continue; }
            if (*p == '"' && !in_sq) { in_dq = !in_dq; continue; }
            if (!in_sq && !in_dq && *p == '=') { eq = p; break; }
            if (!in_sq && !in_dq && isspace((unsigned char)*p)) break;
        }
        if (!eq) break;

        int name_len = (int)(eq - cmd_start);
        if (name_len == 0) break;

        int valid = 1;
        if (!isalpha((unsigned char)cmd_start[0]) && cmd_start[0] != '_') valid = 0;
        for (int i = 1; i < name_len && valid; i++)
            if (!isalnum((unsigned char)cmd_start[i]) && cmd_start[i] != '_') valid = 0;
        if (!valid) break;

        char name[256];
        if (name_len >= (int)sizeof(name)) break;
        memcpy(name, cmd_start, name_len);
        name[name_len] = '\0';

        char *val_start = eq + 1;
        char value[1024];
        int v_idx = 0;

        while (*val_start && !isspace((unsigned char)*val_start) && v_idx < (int)sizeof(value) - 1) {
            if (*val_start == '\'' || *val_start == '"') {
                char q = *val_start++;
                while (*val_start && *val_start != q && v_idx < (int)sizeof(value) - 1) {
                    if (*val_start == '\\' && val_start[1]) { val_start++; value[v_idx++] = *val_start++; continue; }
                    value[v_idx++] = *val_start++;
                }
                if (*val_start == q) val_start++;
            } else if (*val_start == '\\' && val_start[1]) {
                val_start++;
                value[v_idx++] = *val_start++;
            } else {
                value[v_idx++] = *val_start++;
            }
        }
        value[v_idx] = '\0';

        char expanded_value[1024];
        expand_variable(value, expanded_value, sizeof(expanded_value));

        const char *old = getenv(name);
        env_save[env_count][0] = strdup(name);
        env_save[env_count][1] = old ? strdup(old) : NULL;
        env_count++;
        setenv(name, expanded_value, 1);

        cmd_start = val_start;
        while (*cmd_start == ' ' || *cmd_start == '\t') cmd_start++;
    }

    char *args[MAX_ARGS];
    int argc = tokenize(cmd_start, args);

    int has_command = (argc > 0);

    if (argc == 0) {
        for (int i = 0; i < env_count; i++) {
            free(env_save[i][0]);
            if (env_save[i][1]) free(env_save[i][1]);
        }
        free_args(args);
        return;
    }

    if (argc > 0) {
        redir_in_fd = -1;
        redir_out_fd = -1;
        if (redir_in_file[0]) {
            redir_in_fd = open(redir_in_file, O_RDONLY);
            if (redir_in_fd < 0) {
                fprintf(stderr, "nsh: %s: %s\n", redir_in_file, strerror(errno));
                last_exit_status = 1;
                for (int i = 0; i < env_count; i++) {
                    if (env_save[i][1]) { setenv(env_save[i][0], env_save[i][1], 1); free(env_save[i][1]); }
                    else unsetenv(env_save[i][0]);
                    free(env_save[i][0]);
                }
                free_args(args);
                return;
            }
        }
        if (redir_out_file[0]) {
            int flags = O_WRONLY | O_CREAT | (redir_out_append ? O_APPEND : O_TRUNC);
            redir_out_fd = open(redir_out_file, flags, 0644);
            if (redir_out_fd < 0) {
                fprintf(stderr, "nsh: %s: %s\n", redir_out_file, strerror(errno));
                last_exit_status = 1;
                if (redir_in_fd >= 0) close(redir_in_fd);
                for (int i = 0; i < env_count; i++) {
                    if (env_save[i][1]) { setenv(env_save[i][0], env_save[i][1], 1); free(env_save[i][1]); }
                    else unsetenv(env_save[i][0]);
                    free(env_save[i][0]);
                }
                free_args(args);
                return;
            }
        }
        exec_simple(args, argc);
        if (redir_in_fd >= 0) close(redir_in_fd);
        if (redir_out_fd >= 0) close(redir_out_fd);
        redir_in_fd = -1;
        redir_out_fd = -1;
    }

    for (int i = 0; i < env_count; i++) {
        if (env_save[i][1]) { setenv(env_save[i][0], env_save[i][1], 1); free(env_save[i][1]); }
        else unsetenv(env_save[i][0]);
        free(env_save[i][0]);
    }

    free_args(args);
}

static int skip_compound(char *line, int start) {
    char *p = line + start;
    int in_sq = 0, in_dq = 0;
    int depth = 0;

    const char *open_kw = NULL;
    const char *close_kw = NULL;
    int open_len = 0, close_len = 0;

    while (*p == ' ' || *p == '\t') p++;

    if (strncmp(p, "if", 2) == 0 && (p[2] == ' ' || p[2] == '\t' || p[2] == '\n' || p[2] == ';')) {
        open_kw = "if"; close_kw = "fi"; open_len = 2; close_len = 2;
    } else if (strncmp(p, "for", 3) == 0 && (p[3] == ' ' || p[3] == '\t' || p[3] == '\n' || p[3] == ';')) {
        open_kw = "for"; close_kw = "done"; open_len = 3; close_len = 4;
    } else if (strncmp(p, "while", 5) == 0 && (p[5] == ' ' || p[5] == '\t' || p[5] == '\n' || p[5] == ';')) {
        open_kw = "while"; close_kw = "done"; open_len = 5; close_len = 4;
    } else if (strncmp(p, "until", 5) == 0 && (p[5] == ' ' || p[5] == '\t' || p[5] == '\n' || p[5] == ';')) {
        open_kw = "until"; close_kw = "done"; open_len = 5; close_len = 4;
    } else if (strncmp(p, "case", 4) == 0 && (p[4] == ' ' || p[4] == '\t' || p[4] == '\n' || p[4] == ';')) {
        open_kw = "case"; close_kw = "esac"; open_len = 4; close_len = 4;
    }

    if (!open_kw) return 0;

    depth = 1;
    p += open_len;
    while (*p) {
        if (*p == '\\' && !in_sq) { p++; if (*p) p++; continue; }
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; p++; continue; }
        if (*p == '"' && !in_sq) { in_dq = !in_dq; p++; continue; }
        if (in_sq || in_dq) { p++; continue; }

        if (strncmp(p, open_kw, open_len) == 0 &&
            (p[open_len] == ' ' || p[open_len] == '\t' || p[open_len] == '\n' || p[open_len] == ';'))
            depth++;
        if (strncmp(p, close_kw, close_len) == 0 &&
            (p[close_len] == ' ' || p[close_len] == '\t' || p[close_len] == '\n' || p[close_len] == '\0' || p[close_len] == ';')) {
            depth--;
            if (depth == 0) {
                char *end = p + close_len;
                while (*end && *end != ';' && *end != '\n') end++;
                return (int)(end - line);
            }
        }
        p++;
    }
    return 0;
}

static void parse_and_execute(char *cmdline) {
    if (!cmdline || !*cmdline) return;

    char segments[64][ARG_SIZE];
    int seg_count = 0;
    char *p = cmdline;

    while (*p && seg_count < 64) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;

        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }

        char buf[ARG_SIZE];
        int b_idx = 0;
        int in_sq = 0, in_dq = 0;

        int compound_end = 0;
        char *cp = p;
        while (*cp == ' ' || *cp == '\t') cp++;

        char *line_end = cp;
        { char *le = cp; while (*le && *le != '\n' && *le != ';') le++; line_end = le; }
        int line_len = (int)(line_end - cp);

        if (strncmp(cp, "function ", 9) == 0 ||
            (memchr(cp, '(', line_len) && memchr(cp, ')', line_len) && memmem(cp, line_len, "{", 1))) {
            char *brace = memchr(cp, '{', line_len);
            if (brace) {
                int depth = 1;
                char *q = brace + 1;
                int in_sq2 = 0, in_dq2 = 0;
                while (*q && depth > 0) {
                    if (*q == '\\' && !in_sq2) { q++; if (*q) q++; continue; }
                    if (*q == '\'' && !in_dq2) { in_sq2 = !in_sq2; q++; continue; }
                    if (*q == '"' && !in_sq2) { in_dq2 = !in_dq2; q++; continue; }
                    if (!in_sq2 && !in_dq2) {
                        if (*q == '{') depth++;
                        if (*q == '}') depth--;
                    }
                    if (depth > 0) q++;
                }
                if (depth == 0) {
                    char fname[64] = "";
                    char *fn_start = cp;
                    if (strncmp(cp, "function ", 9) == 0) fn_start = cp + 9;
                    char *open_paren = strchr(fn_start, '(');
                    if (open_paren) {
                        char *end_name = open_paren - 1;
                        while (end_name > fn_start && (*end_name == ' ' || *end_name == '\t')) end_name--;
                        int nlen = (int)(end_name - fn_start + 1);
                        if (nlen > 0 && nlen < (int)sizeof(fname)) {
                            strncpy(fname, fn_start, nlen);
                            fname[nlen] = '\0';
                        }
                    } else {
                        char *sp = strpbrk(fn_start, " \t{");
                        if (sp) {
                            int nlen = (int)(sp - fn_start);
                            if (nlen > 0 && nlen < (int)sizeof(fname)) {
                                strncpy(fname, fn_start, nlen);
                                fname[nlen] = '\0';
                            }
                        }
                    }
                    if (fname[0]) {
                        char fbody[4096] = "";
                        int flen = (int)(q - brace - 1);
                        if (flen > 0 && flen < (int)sizeof(fbody)) {
                            strncpy(fbody, brace + 1, flen);
                            fbody[flen] = '\0';
                        }
                        int found = 0;
                        for (int fi2 = 0; fi2 < function_count; fi2++) {
                            if (strcmp(functions[fi2].name, fname) == 0) {
                                strncpy(functions[fi2].body, fbody, sizeof(functions[fi2].body) - 1);
                                functions[fi2].body[sizeof(functions[fi2].body) - 1] = '\0';
                                found = 1;
                                break;
                            }
                        }
                        if (!found && function_count < MAX_FUNCTIONS) {
                            strncpy(functions[function_count].name, fname, sizeof(functions[function_count].name) - 1);
                            functions[function_count].name[sizeof(functions[function_count].name) - 1] = '\0';
                            strncpy(functions[function_count].body, fbody, sizeof(functions[function_count].body) - 1);
                            functions[function_count].body[sizeof(functions[function_count].body) - 1] = '\0';
                            function_count++;
                        }
                    }
                    char *end = q + 1;
                    while (*end && *end != ';' && *end != '\n') end++;
                    p = end;
                    continue;
                }
            }
        }

        if (strncmp(cp, "if ", 3) == 0 || strncmp(cp, "if\t", 3) == 0 || strncmp(cp, "if\n", 3) == 0 || strcmp(cp, "if") == 0 ||
            strncmp(cp, "for ", 4) == 0 || strncmp(cp, "for\t", 4) == 0 || strncmp(cp, "for\n", 4) == 0 || strcmp(cp, "for") == 0 ||
            strncmp(cp, "while ", 6) == 0 || strncmp(cp, "while\t", 6) == 0 || strcmp(cp, "while") == 0 ||
            strncmp(cp, "until ", 6) == 0 || strncmp(cp, "until\t", 6) == 0 || strcmp(cp, "until") == 0 ||
            strncmp(cp, "case ", 5) == 0 || strncmp(cp, "case\t", 5) == 0 || strcmp(cp, "case") == 0) {

            compound_end = skip_compound(cmdline, (int)(cp - cmdline));
        }

        if (compound_end > 0) {
            char *end = cmdline + compound_end;
            int len = (int)(end - p);
            if (len >= ARG_SIZE) len = ARG_SIZE - 1;
            memcpy(buf, p, len);
            buf[len] = '\0';
            p = end;
            strncpy(segments[seg_count], buf, ARG_SIZE - 1);
            segments[seg_count][ARG_SIZE - 1] = '\0';
            seg_count++;
            continue;
        }

        while (*p) {
            if (*p == '\\' && !in_sq) { buf[b_idx++] = *p++; if (*p) buf[b_idx++] = *p++; continue; }
            if (*p == '\'' && !in_dq) { in_sq = !in_sq; buf[b_idx++] = *p++; continue; }
            if (*p == '"' && !in_sq) { in_dq = !in_dq; buf[b_idx++] = *p++; continue; }
            if (!in_sq && !in_dq && *p == '#') {
                while (*p && *p != '\n') p++;
                break;
            }
            if (!in_sq && !in_dq && (*p == ';' || *p == '\n')) break;
            buf[b_idx++] = *p++;
        }
        buf[b_idx] = '\0';
        if (b_idx > 0) {
            strncpy(segments[seg_count], buf, ARG_SIZE - 1);
            segments[seg_count][ARG_SIZE - 1] = '\0';
            seg_count++;
        }
        if (*p == ';' || *p == '\n') p++;
    }

    if (seg_count == 0) return;

    for (int i = 0; i < seg_count; i++) {
        expand_alias(segments[i], ARG_SIZE);
        expand_cmd_subst(segments[i], ARG_SIZE);
        execute_segment(segments[i]);
        if (loop_break_flag || loop_continue_flag) break;
    }
}

int main(int argc, char *argv[]) {
    char self_path[512];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len > 0) {
        self_path[len] = '\0';
        setenv("SHELL", self_path, 1);
    }

    if (argc == 2 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
        printf("Nsh version 2.0\n");
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        char cmd[ARG_SIZE];
        strncpy(cmd, argv[2], ARG_SIZE - 1);
        cmd[ARG_SIZE - 1] = '\0';
        expand_cmd_subst(cmd, sizeof(cmd));
        parse_and_execute(cmd);
        return last_exit_status;
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
                for (int i = 1; i < MAX_HISTORY; i++)
                    strcpy(history[i - 1], history[i]);
                strncpy(history[MAX_HISTORY - 1], buffer, MAX_LINE - 1);
            }

            save_history();

            parse_and_execute(buffer);
        }
    }

    return 0;
}
