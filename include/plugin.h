// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#ifndef PLUGIN_H
#define PLUGIN_H

#define MAX_PLUGINS 32

typedef enum {
    HOOK_PROMPT_LEFT  = 0,
    HOOK_PROMPT_RIGHT = 1,
    HOOK_PRE_EXEC     = 2,
    HOOK_POST_EXEC    = 3,
    HOOK_COUNT        = 4
} HookType;

typedef struct {
    const char *name;
    const char *version;
    char *(*on_prompt_left)(void);
    char *(*on_prompt_right)(void);
    void  (*on_pre_exec)(const char *cmd);
    void  (*on_post_exec)(const char *cmd, int exit_status);
} Plugin;

/* plugin.c */
void  plugins_init(const char *plugin_dir);
void  plugins_unload(void);

char *hook_prompt_left(void);
char *hook_prompt_right(void);
void  hook_pre_exec(const char *cmd);
void  hook_post_exec(const char *cmd, int exit_status);

#endif