#include "exec.h"
#include "term.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_ARGS 64

static int parse_line(char *line, char **args) {
    int arg_count = 0;
    char *p = line;

    while (*p && arg_count < MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == '\0') break;

        char *arg_start = malloc(strlen(p) + 1);
        int buf_idx = 0;
        char in_quote = 0;

        while (*p) {
            if (!in_quote && (*p == '\'' || *p == '"')) {
                in_quote = *p;
            } else if (in_quote && *p == in_quote) {
                in_quote = 0;
            } else if (!in_quote && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
                break;
            } else {
                arg_start[buf_idx++] = *p;
            }
            p++;
        }
        arg_start[buf_idx] = '\0';
        args[arg_count++] = arg_start;
    }
    args[arg_count] = NULL;
    return arg_count;
}

static void free_args(char **args) {
    for (int i = 0; args[i] != NULL; i++) {
        free(args[i]);
    }
}

static void expand_tildes(char **args) {
    char *home = getenv("HOME");
    if (!home) return;

    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "~") == 0) {
            free(args[i]);
            args[i] = strdup(home);
        }
        else if (args[i][0] == '~' && args[i][1] == '/') {
            char expanded[1024];
            snprintf(expanded, sizeof(expanded), "%s%s", home, args[i] + 1);
            free(args[i]);
            args[i] = strdup(expanded);
        }
    }
}

static void handle_export(char **args) {
    if (!args[1]) return;
    char *eq = strchr(args[1], '=');
    if (eq) {
        *eq = '\0';
        setenv(args[1], eq + 1, 1);
    }
}

int process_command(char *line, char **envp) {
    char *args[MAX_ARGS];
    int arg_count = parse_line(line, args);

    if (arg_count == 0) return 0;

    expand_tildes(args);

    if (strcmp(args[0], "exit") == 0) {
        free_args(args);
        return 1;
    }

    if (strcmp(args[0], "cd") == 0) {
        char *target = args[1];
        if (!target) {
            target = getenv("HOME");
        }
        if (target && chdir(target) != 0) {
            perror("cd");
        }
        free_args(args);
        return 0;
    }

    if (strcmp(args[0], "export") == 0) {
        handle_export(args);
        free_args(args);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        free_args(args);
        return 0;
    }

    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        if (strchr(args[0], '/')) {
            execve(args[0], args, envp);
        } else {
            char *path_env = getenv("PATH");
            if (path_env) {
                char *path_copy = strdup(path_env);
                if (path_copy) {
                    char *dir_path = strtok(path_copy, ":");
                    char path_buf[512];
                    while (dir_path) {
                        snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, args[0]);
                        execve(path_buf, args, envp);
                        dir_path = strtok(NULL, ":");
                    }
                    free(path_copy);
                }
            }
        }
        write(2, "Command not found\n", 18);
        _exit(127);
    } else {
        int status;
        waitpid(pid, &status, 0);
    }

    free_args(args);
    return 0;
}
