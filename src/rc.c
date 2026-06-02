// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#include "../include/rc.h"
#include "../include/alias.h"
#include "../include/shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int last_exit_status;
extern int execute_list(CmdList *list);
extern void cmdlist_free(CmdList *list);
extern Token *lex(const char *input, int *ntokens);
extern void tokens_free(Token *toks, int n);
extern CmdList *parse_list(Token *toks, int ntokens);
extern Token *glob_expand_tokens(Token *toks, int *ntokens, int last_exit);

void rc_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return; /* file may not exist, return silently */

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        /* 1. Strip trailing newline and whitespace */
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == ' ' || line[len-1] == '\t')) {
            line[len-1] = '\0';
            len--;
        }

        /* 2. Skip empty lines */
        if (len == 0) continue;

        /* 3. Skip lines starting with '#' (comments) */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#') continue;

        /* 4. Handle alias definition */
        if (strncmp(p, "alias ", 6) == 0) {
            char *alias_part = p + 6;
            char *eq = strchr(alias_part, '=');
            if (eq) {
                /* Find NAME (between "alias " and '=') */
                *eq = '\0';
                char *name = alias_part;
                /* Strip leading/trailing whitespace from name */
                while (*name == ' ' || *name == '\t') name++;
                char *name_end = eq - 1;
                while (name_end > name && (*name_end == ' ' || *name_end == '\t')) name_end--;
                *(name_end + 1) = '\0';

                /* VALUE is everything after '=' */
                char *value = eq + 1;
                /* Strip leading whitespace */
                while (*value == ' ' || *value == '\t') value++;
                
                /* Strip surrounding quotes (single or double) */
                while ((*value == '\'' || *value == '"')) {
                    char quote = *value;
                    int vlen = strlen(value);
                    if (vlen >= 2 && value[vlen-1] == quote) {
                        value++;
                        value[strlen(value)-1] = '\0';
                    } else {
                        break;
                    }
                }
                
                alias_add(name, value);
            }
            continue; /* don't execute as shell command */
        }

        /* 5. Otherwise: execute as shell command */
        int ntokens;
        Token *tokens = lex(p, &ntokens);
        if (!tokens) continue;
        tokens = glob_expand_tokens(tokens, &ntokens, last_exit_status);
        if (!tokens) continue;
        CmdList *list = parse_list(tokens, ntokens);
        if (list) {
            execute_list(list);
            cmdlist_free(list);
        }
        tokens_free(tokens, ntokens);
    }

    fclose(f);
}
