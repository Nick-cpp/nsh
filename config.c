#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

Alias aliases[MAX_ALIASES];
int alias_count = 0;

Function functions[MAX_FUNCTIONS];
int function_count = 0;

static char* trim_whitespace(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static void strip_quotes(char *str) {
    size_t len = strlen(str);
    if (len >= 2 && ((str[0] == '"' && str[len - 1] == '"') || (str[0] == '\'' && str[len - 1] == '\''))) {
        memmove(str, str + 1, len - 2);
        str[len - 2] = '\0';
    }
}

static void parse_export_or_var(char *line) {
    if (strncmp(line, "export ", 7) == 0) {
        line += 7;
    }
    line = trim_whitespace(line);

    char *eq = strchr(line, '=');
    if (!eq) return;

    *eq = '\0';
    char *key = trim_whitespace(line);
    char *val = trim_whitespace(eq + 1);

    strip_quotes(val);
    setenv(key, val, 1);
}

static void parse_alias(char *line) {
    line += 5;
    line = trim_whitespace(line);

    char *eq = strchr(line, '=');
    if (!eq) return;

    *eq = '\0';
    char *name = trim_whitespace(line);
    char *val = trim_whitespace(eq + 1);

    strip_quotes(val);

    if (alias_count < MAX_ALIASES) {
        
        strncpy(aliases[alias_count].name, name, sizeof(aliases[alias_count].name) - 1);
        aliases[alias_count].name[sizeof(aliases[alias_count].name) - 1] = '\0';
        
        strncpy(aliases[alias_count].value, val, sizeof(aliases[alias_count].value) - 1);
        aliases[alias_count].value[sizeof(aliases[alias_count].value) - 1] = '\0';
        
        alias_count++;
    }
}

static void parse_function_header(char *line) {
    char tmp[256];
    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *paren = strchr(tmp, '(');
    if (paren) {
        *paren = '\0';
        char *name = trim_whitespace(tmp);
        if (strlen(name) > 0 && !strchr(name, ' ') && !strchr(name, '\t')) {
            if (function_count < MAX_FUNCTIONS) {
                strncpy(functions[function_count].name, name, sizeof(functions[function_count].name) - 1);
                functions[function_count].name[sizeof(functions[function_count].name) - 1] = '\0';
                function_count++;
            }
        }
    } else if (strncmp(tmp, "function ", 9) == 0) {
        char *name = tmp + 9;
        name = trim_whitespace(name);
        char *space = strpbrk(name, " \t({");
        if (space) *space = '\0';
        if (strlen(name) > 0) {
            if (function_count < MAX_FUNCTIONS) {
                strncpy(functions[function_count].name, name, sizeof(functions[function_count].name) - 1);
                functions[function_count].name[sizeof(functions[function_count].name) - 1] = '\0';
                function_count++;
            }
        }
    }
}

void load_bash_config(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') continue;

        if (strncmp(trimmed, "alias ", 6) == 0) {
            parse_alias(trimmed);
        } else if (strchr(trimmed, '(') || strncmp(trimmed, "function ", 9) == 0) {
            parse_function_header(trimmed);
        } else if (strncmp(trimmed, "export ", 7) == 0 || (strchr(trimmed, '=') && !strchr(trimmed, '('))) {
            parse_export_or_var(trimmed);
        }
    }
    fclose(f);
}

const char* get_alias(const char *name) {
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            return aliases[i].value;
        }
    }
    return NULL;
}

int is_function(const char *name) {
    for (int i = 0; i < function_count; i++) {
        if (strcmp(functions[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

void expand_alias(char *buffer, size_t max_len) {
    
    char temp[1024]; 
    strncpy(temp, buffer, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    char *first_word = strtok(temp, " \t\n");
    if (!first_word) return;

    const char *val = get_alias(first_word);
    if (val) {
        char *rest = buffer + strlen(first_word);
        
        
        char expanded[1024]; 
        
        
        snprintf(expanded, sizeof(expanded), "%s%s", val, rest); 
        
        strncpy(buffer, expanded, max_len - 1);
        buffer[max_len - 1] = '\0';
    }
}
