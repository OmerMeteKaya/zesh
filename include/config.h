#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    /* Prompt */
    int   prompt_show_time;
    int   prompt_show_user;
    char  prompt_color_ok[16];    /* green, cyan, white... */
    char  prompt_color_err[16];

    /* History */
    int   history_max;
    int   history_dedup;
    
    /* Security */
    int   security_warn;
    int   security_block;
    int   security_audit;
    char  security_audit_log[256];

    /* Panel */
    int   panel_max_rows;
    int   panel_max_items;
    int   panel_enabled;

    /* Completion */
    int   completion_enabled;
    int   suggestion_enabled;

    /* Syntax highlight */
    int   highlight_enabled;
} ShellConfig;

/* Global config instance */
extern ShellConfig g_config;

void config_load(const char *path);
void config_save(const char *path);  /* mevcut config'i yaz */

#endif