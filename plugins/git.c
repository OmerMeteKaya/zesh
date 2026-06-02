// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#include "../include/plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static char *get_git_branch(void) {
    FILE *fp = popen("git symbolic-ref --short HEAD 2>/dev/NULL", "r");
    if (!fp) return NULL;

    char buf[256] = {0};
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return NULL;
    }
    pclose(fp);

    /* strip newline */
    buf[strcspn(buf, "\n")] = '\0';
    if (!buf[0]) return NULL;
    return strdup(buf);
}

static int get_git_dirty(void) {
    FILE *fp = popen("git status --porcelain 2>/dev/NULL", "r");
    if (!fp) return 0;

    char buf[4];
    int dirty = (fgets(buf, sizeof(buf), fp) != NULL);
    pclose(fp);
    return dirty;
}

static int get_git_staged_count(void) {
    FILE *fp = popen(
        "git diff --cached --name-only 2>/dev/NULL | wc -l", "r");
    if (!fp) return 0;
    int n = 0;
    fscanf(fp, "%d", &n);
    pclose(fp);
    return n;
}

static int get_git_untracked_count(void) {
    FILE *fp = popen(
        "git ls-files --others --exclude-standard 2>/dev/NULL | wc -l",
        "r");
    if (!fp) return 0;
    int n = 0;
    fscanf(fp, "%d", &n);
    pclose(fp);
    return n;
}

static int get_git_stash_count(void) {
    FILE *fp = popen("git stash list 2>/dev/NULL | wc -l", "r");
    if (!fp) return 0;
    int n = 0;
    fscanf(fp, "%d", &n);
    pclose(fp);
    return n;
}

static void get_git_ahead_behind(int *ahead, int *behind) {
    *ahead = 0;
    *behind = 0;

    FILE *fp = popen(
        "git rev-list --count --left-right @{upstream}...HEAD 2>/dev/NULL",
        "r");
    if (!fp) return;

    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), fp))
        sscanf(buf, "%d\t%d", behind, ahead);
    pclose(fp);
}

static char *git_prompt_left(void) {
    char *branch = get_git_branch();
    if (!branch) return NULL;

    int dirty   = get_git_dirty();
    int staged  = get_git_staged_count();
    int untrack = get_git_untracked_count();
    int stashes = get_git_stash_count();
    int ahead = 0, behind = 0;
    get_git_ahead_behind(&ahead, &behind);

    /* branch color: green=clean, yellow=dirty */
    const char *bc = dirty ? "\033[1;33m" : "\033[1;32m";

    char buf[512];
    snprintf(buf, sizeof(buf), "%s(%s)\033[0m", bc, branch);

    /* staged: green + */
    if (staged > 0) {
        char s[32];
        snprintf(s, sizeof(s), " \033[32m+%d\033[0m", staged);
        strncat(buf, s, sizeof(buf)-strlen(buf)-1);
    }
    /* dirty unstaged: red ~ */
    if (dirty) {
        char s[32];
        snprintf(s, sizeof(s), " \033[31m~%d\033[0m",
                 dirty);  /* get_git_dirty returns 1, not count */
        strncat(buf, s, sizeof(buf)-strlen(buf)-1);
    }
    /* untracked: dim ? */
    if (untrack > 0) {
        char s[32];
        snprintf(s, sizeof(s), " \033[2;37m?%d\033[0m", untrack);
        strncat(buf, s, sizeof(buf)-strlen(buf)-1);
    }
    /* ahead/behind */
    if (ahead > 0) {
        char s[32];
        snprintf(s, sizeof(s), " \033[32m↑%d\033[0m", ahead);
        strncat(buf, s, sizeof(buf)-strlen(buf)-1);
    }
    if (behind > 0) {
        char s[32];
        snprintf(s, sizeof(s), " \033[31m↓%d\033[0m", behind);
        strncat(buf, s, sizeof(buf)-strlen(buf)-1);
    }
    /* stash */
    if (stashes > 0) {
        char s[32];
        snprintf(s, sizeof(s), " \033[35m$%d\033[0m", stashes);
        strncat(buf, s, sizeof(buf)-strlen(buf)-1);
    }

    free(branch);
    return strdup(buf);
}

static Plugin git_plugin = {
    .name            = "git",
    .version         = "1.0",
    .on_prompt_left  = git_prompt_left,
    .on_prompt_right = NULL,
    .on_pre_exec     = NULL,
    .on_post_exec    = NULL,
};

Plugin *plugin_register(void) {
    return &git_plugin;
}
