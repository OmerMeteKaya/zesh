// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya



#ifndef INPUT_H
#define INPUT_H

#define MAXIMUM_INPUT 4096


char *read_line(const char *prompt);

/* history.c */
void  history_init(const char *db_path);
void  history_add(const char *line);
char *history_get(int offset);
char *history_search_prefix(const char *prefix);
char *history_search(const char *query, int skip);
char **history_search_multi(const char *query, int max_results, int *count_out, int **ids_out);
int   history_count(void);
int   history_total_count(void);
void  history_close(void);

/* Read here-doc content until delimiter.
   Returns malloc'd string with all lines joined by \n.
   expand=1: expand variables, expand=0: literal */
char *read_heredoc(const char *delimiter, int expand);

#endif

