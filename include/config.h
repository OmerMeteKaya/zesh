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

    /* Highlight colors — ANSI color codes */
    char  hl_color_keyword[16];   /* if/while/for etc.  — default: magenta */
    char  hl_color_string[16];    /* "str" 'str'        — default: green   */
    char  hl_color_variable[16];  /* $var               — default: yellow  */
    char  hl_color_comment[16];   /* # comment          — default: dim     */
    char  hl_color_operator[16];  /* | > & ;            — default: bold    */
    char  hl_color_cmd_ok[16];    /* found command      — default: green   */
    char  hl_color_cmd_err[16];   /* not found          — default: red     */
    char  hl_color_path[16];      /* path/file          — default: cyan    */
    char  hl_color_flag[16];      /* -flag              — default: yellow  */
} ShellConfig;

/* Global config instance */
extern ShellConfig g_config;

void config_load(const char *path);
void config_save(const char *path);

#endif