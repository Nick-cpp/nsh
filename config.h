#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#define MAX_ALIASES 128
#define MAX_FUNCTIONS 128

typedef struct {
    char name[64];
    char value[256];
} Alias;

typedef struct {
    char name[64];
} Function;

extern Alias aliases[MAX_ALIASES];
extern int alias_count;

extern Function functions[MAX_FUNCTIONS];
extern int function_count;

void load_bash_config(const char *filepath);
const char* get_alias(const char *name);
int is_function(const char *name);
void expand_alias(char *buffer, size_t max_len);

#endif
