// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#include "../include/alias.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef USE_RUST_ALIAS
/* Route the alias table through the Rust implementation (alias.rs). The full
 * C implementation below is preserved verbatim under #else. */
#include "../include/zesh_rs.h"

void  alias_init(void)                            { alias_init_rs(); }
void  alias_add(const char *name, const char *v)  { alias_add_rs(name, v); }
void  alias_remove(const char *name)              { alias_remove_rs(name); }
char *alias_expand(const char *name)              { return alias_expand_rs(name); }
void  alias_list(void)                            { alias_list_rs(); }
void  alias_free(void)                            { alias_free_rs(); }
void  alias_each(void (*cb)(const char *name, const char *value, void *ud),
                 void *ud)                        { alias_each_rs(cb, ud); }

#else /* !USE_RUST_ALIAS — original C implementation */

static Alias table[MAX_ALIASES];
static int   alias_count = 0;

void alias_remove(const char *name) {
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (table[i].active && table[i].name &&
            strcmp(table[i].name, name) == 0) {
            free(table[i].name);
            free(table[i].value);
            table[i].name = NULL;
            table[i].value = NULL;
            table[i].active = 0;
            alias_count--;
            return;
            }
    }
}

void alias_init(void) {
    memset(table, 0, sizeof(table));
    alias_count = 0;
}

void alias_add(const char *name, const char *value) {
    if (!name || !value) return;
    
    // Search existing aliases
    for (int i = 0; i < alias_count; i++) {
        if (table[i].active && strcmp(table[i].name, name) == 0) {
            free(table[i].value);
            table[i].value = strdup(value);
            return;
        }
    }
    
    // Check if we have space for a new alias
    if (alias_count >= MAX_ALIASES) {
        fprintf(stderr, "alias: maximum number of aliases reached\n");
        return;
    }
    
    // Add new entry
    table[alias_count].name = strdup(name);
    table[alias_count].value = strdup(value);
    table[alias_count].active = 1;
    alias_count++;
}

char *alias_expand(const char *name) {
    if (!name) return NULL;
    
    // Search table for active entry matching name
    for (int i = 0; i < alias_count; i++) {
        if (table[i].active && strcmp(table[i].name, name) == 0) {
            return table[i].value;
        }
    }
    
    return NULL;
}

void alias_list(void) {
    // Print all active aliases
    for (int i = 0; i < alias_count; i++) {
        if (table[i].active) {
            printf("alias %s='%s'\n", table[i].name, table[i].value);
        }
    }
}

void alias_free(void) {
    // Free all active entries
    for (int i = 0; i < alias_count; i++) {
        if (table[i].active) {
            free(table[i].name);
            free(table[i].value);
        }
    }
    
    // Reset table
    memset(table, 0, sizeof(table));
    alias_count = 0;
}

void alias_each(void (*cb)(const char *name, const char *value, void *ud), void *ud) {
    if (!cb) return;
    
    // Iterate through all active entries
    for (int i = 0; i < alias_count; i++) {
        if (table[i].active && table[i].name && table[i].value) {
            cb(table[i].name, table[i].value, ud);
        }
    }
}

#endif /* USE_RUST_ALIAS */
