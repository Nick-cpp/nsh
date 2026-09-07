#ifndef TERM_H
#define TERM_H

#include <termios.h>

#define MAX_LINE 1024
#define MAX_HISTORY 100

extern char history[MAX_HISTORY][MAX_LINE];
extern int history_count;
extern int old_cursor_rows;

void enable_raw_mode(struct termios *orig);
void disable_raw_mode(struct termios *orig);
int read_line_custom(char *buffer, struct termios *orig, int prompt_len);

#endif
