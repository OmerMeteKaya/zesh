// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#ifndef COMPLETIONS_H
#define COMPLETIONS_H

char **get_subcommands(const char *cmd, const char *word, int *count_out);
char **get_dynamic_completions(const char *cmdline, int cursor_pos, int *count_out);

#endif
