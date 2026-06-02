// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#ifndef ALIAS_H
#define ALIAS_H

#define MAX_ALIASES 128

typedef struct {
    char *name;
    char *value;
    int   active;
} Alias;

void  alias_init(void);
void  alias_remove(const char *name);
void  alias_add(const char *name, const char *value);
char *alias_expand(const char *name);  /* NULL if not found */
void  alias_list(void);
void  alias_free(void);
void  alias_each(void (*cb)(const char *name, const char *value, void *ud), void *ud);

#endif
