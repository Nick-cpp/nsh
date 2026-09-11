#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#define MAX_ALIASES 128
#define MAX_FUNCTIONS 128
#define MAX_COMPLETIONS 64

typedef struct {
    char name[64];
    char value[256];
} Alias;

typedef struct {
    char name[64];
    char body[4096];
} Function;

typedef struct {
    char cmd_name[64];
    char words[1024];
} Completion;

extern Alias aliases[MAX_ALIASES];
extern int alias_count;

extern Function functions[MAX_FUNCTIONS];
extern int function_count;

extern Completion completions[MAX_COMPLETIONS];
extern int completion_count;

void load_bash_config(const char *filepath);
const char* get_alias(const char *name);
int is_function(const char *name);
const char* get_function_body(const char *name);
void expand_alias(char *buffer, size_t max_len);
void add_completion(const char *cmd_name, const char *words);
const char* get_completions(const char *cmd_name);

#endif
