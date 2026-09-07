#include "prompt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static int u8_char_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}


static int u8_visual_len(const char *str, size_t bytes) {
    int vis = 0;
    size_t i = 0;
    while (i < bytes) {
        vis++;
        i += u8_char_len((unsigned char)str[i]);
    }
    return vis;
}

static int print_cwd_formatted(void) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) {
        write(1, "?", 1);
        return 1;
    }

    char *home = getenv("HOME");
    char *to_print = cwd;
    int is_home = 0;

    if (home != NULL) {
        size_t home_len = strlen(home);
        if (strncmp(cwd, home, home_len) == 0) {
            if (cwd[home_len] == '\0' || cwd[home_len] == '/') {
                write(1, "~", 1);
                to_print = cwd + home_len;
                is_home = 1;
            }
        }
    }

    size_t bytes_to_print = strlen(to_print);
    write(1, to_print, bytes_to_print);
    
    
    return (is_home ? 1 : 0) + u8_visual_len(to_print, bytes_to_print);
}

int print_prompt(void) {
    char *ps1 = getenv("PS1");
    int vis_len = 0;
    int in_non_visible = 0; 
    int in_esc = 0;         
    
    if (!ps1) {
        write(1, "# ", 2);
        fflush(stdout);
        return 2;
    }

    size_t len = strlen(ps1);
    for (size_t i = 0; i < len; ) {
        
        if (ps1[i] == '\001') { in_non_visible = 1; i++; continue; }
        if (ps1[i] == '\002') { in_non_visible = 0; i++; continue; }

        if (ps1[i] == '\\' && i + 1 < len) {
            if (ps1[i+1] == '[') { in_non_visible = 1; i += 2; continue; }
            if (ps1[i+1] == ']') { in_non_visible = 0; i += 2; continue; }
            if (ps1[i+1] == 'w') {
                int cw_len = print_cwd_formatted();
                if (!in_non_visible && !in_esc) vis_len += cw_len;
                i += 2; continue;
            }
            if (ps1[i+1] == 'e') {
                char esc = '\033';
                write(1, &esc, 1);
                in_esc = 1;
                i += 2; continue;
            }
            
            if (i + 3 < len && ps1[i+1] == '0' && ps1[i+2] == '3' && ps1[i+3] == '3') {
                char esc = '\033';
                write(1, &esc, 1);
                in_esc = 1;
                i += 4; continue;
            }
            
            char c = ps1[i+1];
            write(1, &c, 1);
            if (!in_non_visible && !in_esc) vis_len++;
            
            if (in_esc && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                in_esc = 0;
            }
            i += 2; continue;
        }
        
        
        if (ps1[i] == '\033') {
            write(1, &ps1[i], 1);
            in_esc = 1;
            i++;
            continue;
        }

        
        int char_bytes = u8_char_len((unsigned char)ps1[i]);
        for (int j = 0; j < char_bytes && i + j < len; j++) {
            write(1, &ps1[i+j], 1);
        }
        
        if (!in_non_visible && !in_esc) {
            vis_len += 1;
        }
        
        if (in_esc && char_bytes == 1) {
            char c = ps1[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                in_esc = 0;
            }
        }
        
        i += char_bytes;
    }

    fflush(stdout);
    return vis_len;
}
