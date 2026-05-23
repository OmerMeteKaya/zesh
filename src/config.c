//
// Created by mete on 3.05.2026.
//
#include "../include/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Global config with defaults */
ShellConfig g_config = {
    .prompt_show_time    = 1,
    .prompt_show_user    = 1,
    .prompt_color_ok     = "green",
    .prompt_color_err    = "red",
    .history_max         = 10000,
    .history_dedup       = 1,
    .panel_max_rows      = 4,
    .panel_max_items     = 60,
    .panel_enabled       = 1,
    .completion_enabled  = 1,
    .suggestion_enabled  = 1,
    .highlight_enabled   = 1,
    .security_warn      = 1,
    .security_block     = 0,
    .security_audit     = 1,
    .security_audit_log = "~/.mysh/audit.log",
    .hl_color_keyword  = "magenta",
    .hl_color_string   = "green",
    .hl_color_variable = "yellow",
    .hl_color_comment  = "dim",
    .hl_color_operator = "bold",
    .hl_color_cmd_ok   = "green",
    .hl_color_cmd_err  = "red",
    .hl_color_path     = "cyan",
    .hl_color_flag     = "yellow",
};

/* Helper: parse_bool */
static int parse_bool(const char *val) {
    return (strcmp(val,"true")==0 || strcmp(val,"1")==0 ||
            strcmp(val,"yes")==0 || strcmp(val,"on")==0);
}

/* Helper: strip */
static char *strip(char *s) {
    while (isspace((unsigned char)*s)) s++;
    int l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l-1])) s[--l] = '\0';
    return s;
}

void config_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;  /* file may not exist — use defaults */

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* strip newline */
        line[strcspn(line, "\n")] = '\0';

        /* skip comments and empty lines */
        char *p = strip(line);
        if (!*p || *p == '#') continue;

        /* split on = */
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = strip(p);
        char *val = strip(eq + 1);

        /* strip surrounding quotes from val */
        if ((*val == '"' || *val == '\'')) {
            char q = *val;
            val++;
            int vl = strlen(val);
            if (vl > 0 && val[vl-1] == q) val[vl-1] = '\0';
        }

        /* match keys */
        if      (strcmp(key,"prompt_show_time")==0)
            g_config.prompt_show_time = parse_bool(val);
        else if (strcmp(key,"prompt_show_user")==0)
            g_config.prompt_show_user = parse_bool(val);
        else if (strcmp(key,"prompt_color_ok")==0)
            strncpy(g_config.prompt_color_ok, val,
                    sizeof(g_config.prompt_color_ok)-1);
        else if (strcmp(key,"prompt_color_err")==0)
            strncpy(g_config.prompt_color_err, val,
                    sizeof(g_config.prompt_color_err)-1);
        else if (strcmp(key,"history_max")==0)
            g_config.history_max = atoi(val);
        else if (strcmp(key,"history_dedup")==0)
            g_config.history_dedup = parse_bool(val);
        else if (strcmp(key,"panel_max_rows")==0)
            g_config.panel_max_rows = atoi(val);
        else if (strcmp(key,"panel_max_items")==0)
            g_config.panel_max_items = atoi(val);
        else if (strcmp(key,"panel_enabled")==0)
            g_config.panel_enabled = parse_bool(val);
        else if (strcmp(key,"completion_enabled")==0)
            g_config.completion_enabled = parse_bool(val);
        else if (strcmp(key,"suggestion_enabled")==0)
            g_config.suggestion_enabled = parse_bool(val);
        else if (strcmp(key,"highlight_enabled")==0)
            g_config.highlight_enabled = parse_bool(val);
        else if (strcmp(key,"hl_color_keyword")==0)
            strncpy(g_config.hl_color_keyword, val, sizeof(g_config.hl_color_keyword)-1);
        else if (strcmp(key,"hl_color_string")==0)
            strncpy(g_config.hl_color_string, val, sizeof(g_config.hl_color_string)-1);
        else if (strcmp(key,"hl_color_variable")==0)
            strncpy(g_config.hl_color_variable, val, sizeof(g_config.hl_color_variable)-1);
        else if (strcmp(key,"hl_color_comment")==0)
            strncpy(g_config.hl_color_comment, val, sizeof(g_config.hl_color_comment)-1);
        else if (strcmp(key,"hl_color_operator")==0)
            strncpy(g_config.hl_color_operator, val, sizeof(g_config.hl_color_operator)-1);
        else if (strcmp(key,"hl_color_cmd_ok")==0)
            strncpy(g_config.hl_color_cmd_ok, val, sizeof(g_config.hl_color_cmd_ok)-1);
        else if (strcmp(key,"hl_color_cmd_err")==0)
            strncpy(g_config.hl_color_cmd_err, val, sizeof(g_config.hl_color_cmd_err)-1);
        else if (strcmp(key,"hl_color_path")==0)
            strncpy(g_config.hl_color_path, val, sizeof(g_config.hl_color_path)-1);
        else if (strcmp(key,"hl_color_flag")==0)
            strncpy(g_config.hl_color_flag, val, sizeof(g_config.hl_color_flag)-1);
    }

    fclose(f);
}

void config_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# mysh configuration\n");
    fprintf(f, "# Generated automatically — edit as needed\n\n");

    fprintf(f, "# Prompt\n");
    fprintf(f, "prompt_show_time=%s\n",
            g_config.prompt_show_time ? "true" : "false");
    fprintf(f, "prompt_show_user=%s\n",
            g_config.prompt_show_user ? "true" : "false");
    fprintf(f, "prompt_color_ok=%s\n",   g_config.prompt_color_ok);
    fprintf(f, "prompt_color_err=%s\n",  g_config.prompt_color_err);

    fprintf(f, "\n# History\n");
    fprintf(f, "history_max=%d\n",       g_config.history_max);
    fprintf(f, "history_dedup=%s\n",
            g_config.history_dedup ? "true" : "false");

    fprintf(f, "\n# Panel\n");
    fprintf(f, "panel_max_rows=%d\n",    g_config.panel_max_rows);
    fprintf(f, "panel_max_items=%d\n",   g_config.panel_max_items);
    fprintf(f, "panel_enabled=%s\n",
            g_config.panel_enabled ? "true" : "false");

    fprintf(f, "\n# Completion\n");
    fprintf(f, "completion_enabled=%s\n",
            g_config.completion_enabled ? "true" : "false");
    fprintf(f, "suggestion_enabled=%s\n",
            g_config.suggestion_enabled ? "true" : "false");

    fprintf(f, "\n# Syntax highlight\n");
    fprintf(f, "highlight_enabled=%s\n",
            g_config.highlight_enabled ? "true" : "false");

    fclose(f);
}
