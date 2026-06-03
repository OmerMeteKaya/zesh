// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

/* GLOB_TILDE is a GNU extension; required when building with
 * -D_POSIX_C_SOURCE which otherwise disables it on glibc. Harmless on
 * macOS/FreeBSD where GLOB_TILDE is available unconditionally. */
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <glob.h>
#ifndef GLOB_TILDE
#define GLOB_TILDE 0
#endif
#include <dirent.h>
#include <ctype.h>
#include "../include/input.h"

#include <sys/stat.h>

#include "../include/highlight.h"
#include "../include/alias.h"
#include "../include/completions.h"
#include "../include/config.h"

/* External declarations */
extern int is_builtin(const char *cmd);
extern char *alias_expand(const char *name);
extern char *history_get(int offset);
extern char *history_search_prefix(const char *prefix);
extern char *history_search(const char *query, int skip);
extern void history_add(const char *line);
extern int history_count(void);
extern char *highlight(const char *buf, int len);

/* ------------------------------------------------------------------ */
/*  ANSI-aware display length                                           */
/*  Skips \033[...m escape sequences when counting columns.            */
/* ------------------------------------------------------------------ */
static int ansi_display_len(const char *s) {
    int cols = 0;
    while (*s) {
        if (*s == '\033') {
            s++; /* skip ESC */
            if (*s == '[') {
                s++; /* skip [ */
                while (*s && !isalpha((unsigned char)*s)) s++;
                if (*s) s++; /* skip final letter */
            }
            continue;
        }
        unsigned char c = (unsigned char)*s;
        if      (c < 0x80) { cols++; s++; }
        else if (c < 0xC0) { s++; }           /* UTF-8 continuation */
        else if (c < 0xE0) { cols++; s += 2; }
        else if (c < 0xF0) { cols++; s += 3; }
        else               { cols++; s += 4; }
    }
    return cols;
}

/* ------------------------------------------------------------------ */
/*  Block-depth tracking                                                */
/* ------------------------------------------------------------------ */
static int ml_depth_delta(const char *line) {
    if (!line) return 0;
    const char *tr = line;
    while (*tr == ' ' || *tr == '\t') tr++;
    if (*tr == '#' || *tr == '\0') return 0;

    int delta = 0;
    static const struct { const char *kw; int d; } kws[] = {
        {"if",+1},{"while",+1},{"until",+1},{"for",+1},{"case",+1},
        {"fi",-1},{"done",-1},{"esac",-1},
        {NULL,0}
    };

    const char *p = tr;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (!*p) break;
        const char *ws = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ';') p++;
        int wlen = (int)(p - ws);
        /* function: word() — counts as +1, skip { on same line */
        if (wlen >= 3 && ws[wlen-2] == '(' && ws[wlen-1] == ')') {
            delta++;
            /* skip the opening { if it appears on the same line */
            const char *q = p;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '{') p = q + 1; /* skip { */
            continue;
        }
        /* standalone { } — function body braces (only if no foo() on this line) */
        if (wlen == 1 && ws[0] == '{') { delta++; continue; }
        if (wlen == 1 && ws[0] == '}') { delta--; continue; }
        for (int i = 0; kws[i].kw; i++) {
            int klen = (int)strlen(kws[i].kw);
            if (wlen == klen && strncmp(ws, kws[i].kw, klen) == 0) { delta += kws[i].d; break; }
        }
    }
    return delta;
}

static int ml_count_depth(const char *buf, int len) {
    char tmp[MAXIMUM_INPUT * 8];
    int copy = len < (int)sizeof(tmp)-1 ? len : (int)sizeof(tmp)-1;
    memcpy(tmp, buf, copy); tmp[copy] = '\0';
    int depth = 0;
    char *line = strtok(tmp, "\n");
    while (line) { depth += ml_depth_delta(line); line = strtok(NULL, "\n"); }
    return depth;
}

/* ------------------------------------------------------------------ */
/*  UTF-8 helpers (original, unchanged)                                 */
/* ------------------------------------------------------------------ */
static int utf8_display_len(const char *s) {
    int cols = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if      (c < 0x80) { cols++; s++; }
        else if (c < 0xC0) { s++; }
        else if (c < 0xE0) { cols++; s += 2; }
        else if (c < 0xF0) { cols++; s += 3; }
        else               { cols++; s += 4; }
    }
    return cols;
}

static int utf8_char_len(const char *buf, int pos) {
    unsigned char c = (unsigned char)buf[pos];
    if (c < 0x80) return 1; if (c < 0xC0) return 1;
    if (c < 0xE0) return 2; if (c < 0xF0) return 3; return 4;
}

static int utf8_prev_char_len(const char *buf, int pos) {
    if (pos <= 0) return 0;
    int back = 1;
    while (back < pos && back < 4 &&
           ((unsigned char)buf[pos - back] & 0xC0) == 0x80) back++;
    return back;
}

static int word_start(const char *buf, int pos) {
    int i = pos - 1;
    while (i > 0 && buf[i-1] != ' ') i--;
    return i;
}

/* ------------------------------------------------------------------ */
/*  Original single-line render (UNCHANGED from original input.c)      */
/* ------------------------------------------------------------------ */
static void render(const char *prompt, const char *buf, int len, int pos) {
    write(STDOUT_FILENO, "\r", 1);
    write(STDOUT_FILENO, prompt, strlen(prompt));

    char tmp_buf[MAXIMUM_INPUT + 1];
    memcpy(tmp_buf, buf, len);
    tmp_buf[len] = '\0';

    char *colored = highlight(tmp_buf, len);
    if (colored) {
        write(STDOUT_FILENO, colored, strlen(colored));
        free(colored);
    } else {
        write(STDOUT_FILENO, buf, len);
    }

    write(STDOUT_FILENO, "\033[K", 3);

    /* move cursor back to pos */
    char tmp[MAXIMUM_INPUT];
    int tail = len - pos;
    if (tail > 0) {
        memcpy(tmp, buf + pos, tail);
        tmp[tail] = '\0';
        int back = utf8_display_len(tmp);
        if (back > 0) {
            char esc[16];
            snprintf(esc, sizeof(esc), "\033[%dD", back);
            write(STDOUT_FILENO, esc, strlen(esc));
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Multiline render                                                     */
/*                                                                      */
/*  The buffer may contain '\n' separating logical lines.               */
/*  Strategy:                                                           */
/*   - Track how many screen lines were drawn last time (ml_prev_rows)  */
/*   - On redraw: go up (ml_prev_rows-1) lines, then render each line   */
/*     with render(), emitting \r\n between lines                       */
/*   - Use \033[J after last line to erase leftover content             */
/*   - Move cursor back up to the correct logical line                  */
/*                                                                      */
/*  For single-line input (no '\n' in buf): behaves exactly like the    */
/*  original render() — zero extra overhead.                            */
/* ------------------------------------------------------------------ */

#define CONT_PROMPT      "> "
#define CONT_PROMPT_ANSI "\033[2;37m> \033[0m"

/*
 * Count how many '\n' are in buf[0..len-1].
 * = number of logical lines minus 1.
 */
static int count_newlines(const char *buf, int len) {
    int n = 0;
    for (int i = 0; i < len; i++) if (buf[i] == '\n') n++;
    return n;
}

/*
 * Full multiline render.
 * ml_prev_rows: screen rows drawn last call (pass 1 on first call).
 * Returns new screen rows drawn.
 */
static int ml_render(const char *prompt, const char *buf, int len,
                      int pos, int ml_prev_rows) {

    /* ── go back to top of input area ─────────────────────────────── */
    if (ml_prev_rows > 1) {
        char esc[16];
        snprintf(esc, sizeof(esc), "\033[%dA", ml_prev_rows - 1);
        write(STDOUT_FILENO, esc, strlen(esc));
    }

    /* ── find which logical line the cursor is on ─────────────────── */
    int cursor_line = 0;
    for (int i = 0; i < pos; i++) if (buf[i] == '\n') cursor_line++;
    int total_lines = count_newlines(buf, len); /* 0-based index of last line */

    /* ── render each logical line ─────────────────────────────────── */
    int seg_start = 0;
    int line_idx  = 0;
    int rows_drawn = 0;

    for (int i = 0; i <= len; i++) {
        if (i < len && buf[i] != '\n') continue;

        int seg_len = i - seg_start;

        /* which prompt to use */
        const char *pfx = (line_idx == 0) ? prompt : CONT_PROMPT_ANSI;

        /* cursor position within this segment */
        int seg_pos;
        if (line_idx < cursor_line) {
            seg_pos = seg_len; /* cursor is on a later line */
        } else if (line_idx == cursor_line) {
            /* cursor offset within this line */
            int line_start = seg_start;
            seg_pos = pos - line_start;
            if (seg_pos < 0) seg_pos = 0;
            if (seg_pos > seg_len) seg_pos = seg_len;
        } else {
            seg_pos = 0; /* cursor is on an earlier line */
        }

        /* use original render() logic for this segment */
        write(STDOUT_FILENO, "\r", 1);
        write(STDOUT_FILENO, pfx, strlen(pfx));

        if (seg_len > 0) {
            char tmp[MAXIMUM_INPUT + 1];
            int cplen = seg_len < MAXIMUM_INPUT ? seg_len : MAXIMUM_INPUT;
            memcpy(tmp, buf + seg_start, cplen); tmp[cplen] = '\0';
            char *colored = highlight(tmp, cplen);
            if (colored) { write(STDOUT_FILENO, colored, strlen(colored)); free(colored); }
            else          { write(STDOUT_FILENO, tmp, cplen); }
        }
        write(STDOUT_FILENO, "\033[K", 3);

        if (i < len) {
            /* not the last line — move to next screen line */
            write(STDOUT_FILENO, "\r\n", 2);
            rows_drawn++;
        }

        line_idx++;
        seg_start = i + 1;
    }

    /* erase leftover lines from previous render (handles shrink) */
    write(STDOUT_FILENO, "\033[J", 3);

    int total_screen_rows = rows_drawn + 1;

    /* ── position cursor on the correct line ──────────────────────── */
    int lines_below_cursor = total_lines - cursor_line;
    if (lines_below_cursor > 0) {
        char esc[16];
        snprintf(esc, sizeof(esc), "\033[%dA", lines_below_cursor);
        write(STDOUT_FILENO, esc, strlen(esc));
    }

    /* ── position cursor at correct column on that line ───────────── */
    write(STDOUT_FILENO, "\r", 1);

    /* find start of cursor line in buf */
    int cursor_line_start = 0;
    {
        int nl = 0;
        for (int i = 0; i < len; i++) {
            if (nl == cursor_line) { cursor_line_start = i; break; }
            if (buf[i] == '\n') nl++;
        }
        if (cursor_line == 0) cursor_line_start = 0;
    }
    int cursor_col = pos - cursor_line_start; /* bytes from line start to cursor */

    /* prompt columns for cursor line */
    int pfx_cols = (cursor_line == 0)
                    ? ansi_display_len(prompt)
                    : (int)strlen(CONT_PROMPT); /* "> " = 2 */

    int target_col = pfx_cols + cursor_col;
    if (target_col > 0) {
        char esc[16];
        snprintf(esc, sizeof(esc), "\033[%dC", target_col);
        write(STDOUT_FILENO, esc, strlen(esc));
    }

    return total_screen_rows;
}

/* ------------------------------------------------------------------ */
/*  Ghost suggestion (original logic, unchanged)                        */
/* ------------------------------------------------------------------ */
static char *find_suggestion(const char *buf, int len) {
    if (len == 0) return NULL;
    char *h = history_search_prefix(buf);
    /* only use history suggestion if it is single-line */
    if (h) {
        if (strchr(h, '\n') != NULL) { free(h); h = NULL; }
        else return h;
    }

    int ws = word_start(buf, len);
    int wlen = len - ws;
    if (wlen < 2) return NULL;

    char pattern[MAXIMUM_INPUT];
    strncpy(pattern, buf + ws, wlen); pattern[wlen] = '\0';
    strncat(pattern, "*", MAXIMUM_INPUT - wlen - 1);

    glob_t g;
    if (glob(pattern, GLOB_MARK|GLOB_TILDE, NULL, &g) == 0 && g.gl_pathc > 0) {
        char *result = strdup(g.gl_pathv[0]); globfree(&g); return result;
    }
    globfree(&g); return NULL;
}

/*
 * render + ghost suggestion.
 * For single-line: calls original render() — identical behavior.
 * For multi-line:  calls ml_render().
 * Returns updated ml_prev_rows.
 */

/*
 * Completely erase the input area (all rendered rows + panel rows).
 * Call before replacing buf with a new history entry.
 * Leaves cursor at column 0 of the first input row.
 */
static void clear_input_area(int *ml_prev_rows, int *panel_rows) {
    /* move down past panel if any */
    if (*panel_rows > 0) {
        char esc[16];
        snprintf(esc, sizeof(esc), "\033[%dB", *panel_rows);
        write(STDOUT_FILENO, esc, strlen(esc));
    }
    /* move up to top of input area */
    int total_up = (*ml_prev_rows - 1) + *panel_rows;
    if (total_up > 0) {
        char esc[16];
        snprintf(esc, sizeof(esc), "\033[%dA", total_up);
        write(STDOUT_FILENO, esc, strlen(esc));
    }
    /* erase from cursor to end of screen */
    write(STDOUT_FILENO, "\r\033[J", 4);
    *ml_prev_rows = 1;
    *panel_rows   = 0;
}

static int render_ml(const char *prompt, const char *buf, int len,
                      int pos, int ml_prev_rows) {

    int has_newline = 0;
    for (int i = 0; i < len; i++) if (buf[i] == '\n') { has_newline = 1; break; }

    int rows;
    if (!has_newline) {
        /* single-line: use original render() exactly */
        render(prompt, buf, len, pos);
        rows = 1;
    } else {
        rows = ml_render(prompt, buf, len, pos, ml_prev_rows);
        /* panel disabled during multiline editing — cursor is mid-buffer */
        return rows;
    }

    /* ghost suggestion — only on last line, cursor at end */
    if (pos != len) return rows;
    for (int i = pos; i < len; i++) if (buf[i] == '\n') return rows;
    if (!g_config.suggestion_enabled) return rows;
    if (len == 0) return rows;

    /* get last logical line */
    int ls = len;
    while (ls > 0 && buf[ls-1] != '\n') ls--;
    int ll = len - ls;
    if (ll <= 0) return rows;

    char last_line[MAXIMUM_INPUT];
    int cplen = ll < MAXIMUM_INPUT-1 ? ll : MAXIMUM_INPUT-1;
    memcpy(last_line, buf + ls, cplen); last_line[cplen] = '\0';

    char *sug = find_suggestion(last_line, ll);
    if (!sug) return rows;
    int sug_len = strlen(sug);
    if (sug_len > ll) {
        const char *ghost = sug + ll;
        int ghost_cols = utf8_display_len(ghost);
        if (ghost_cols > 0) {
            write(STDOUT_FILENO, "\033[2;37m", 7);
            write(STDOUT_FILENO, ghost, strlen(ghost));
            write(STDOUT_FILENO, "\033[0m", 4);
            char esc[16];
            snprintf(esc, sizeof(esc), "\033[%dD", ghost_cols);
            write(STDOUT_FILENO, esc, strlen(esc));
        }
    }
    free(sug);
    return rows;
}

/* Original render_with_suggestion — kept as thin wrapper */
static void render_with_suggestion(const char *prompt, const char *buf,
                                    int len, int pos) {
    if (!g_config.suggestion_enabled) { render(prompt, buf, len, pos); return; }
    render(prompt, buf, len, pos);

    if (pos != len) return;
    char *sug = find_suggestion(buf, len);
    if (!sug) return;
    int sug_len = strlen(sug);
    if (sug_len <= len) { free(sug); return; }
    const char *ghost = sug + len;
    int ghost_cols = utf8_display_len(ghost);
    if (ghost_cols > 0) {
        write(STDOUT_FILENO, "\033[2;37m", 7);
        write(STDOUT_FILENO, ghost, strlen(ghost));
        write(STDOUT_FILENO, "\033[0m", 4);
        char esc[16];
        snprintf(esc, sizeof(esc), "\033[%dD", ghost_cols);
        write(STDOUT_FILENO, esc, strlen(esc));
    }
    free(sug);
}

/* ------------------------------------------------------------------ */
/*  Panel helpers (original, unchanged)                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char **items; int *count; const char *word; int wlen; int cap;
} AliasCbCtx;

static void alias_collect_cb(const char *name, const char *value, void *ud) {
    (void)value;
    AliasCbCtx *ctx = (AliasCbCtx *)ud;
    if (*ctx->count >= ctx->cap) return;
    if (strncmp(name, ctx->word, ctx->wlen) == 0) {
        for (int j = 0; j < *ctx->count; j++)
            if (strcmp(ctx->items[j], name) == 0) return;
        ctx->items[(*ctx->count)++] = strdup(name);
    }
}

static void panel_show_history(int hist_off, int *panel_rows) {
    /* clear previous panel — move down past all rows, erase to end of screen */
    if (*panel_rows > 0) {
        for (int i = 0; i < *panel_rows; i++)
            write(STDOUT_FILENO, "\033[B", 3);
        write(STDOUT_FILENO, "\033[J", 3);  /* erase from cursor to end */
        char esc_up[16];
        snprintf(esc_up, sizeof(esc_up), "\033[%dA", *panel_rows);
        write(STDOUT_FILENO, esc_up, strlen(esc_up));
        *panel_rows = 0;
    }

    char *entries[10]; int count = 0;
    for (int i = 1; i <= 10; i++) {
        char *h = history_get(i); if (!h) break; entries[count++] = h;
    }
    if (count == 0) return;

    int total = history_total_count();
    int rows = 0;

    for (int i = 0; i < count; i++) {
        int is_sel = (i+1 == hist_off);
        int abs_index = total - i;
        char idx_str[32];

        /* count screen lines this entry occupies (newlines + 1) */
        int entry_lines = 1;
        for (const char *p = entries[i]; *p; p++)
            if (*p == '\n') entry_lines++;

        /* first line of entry */
        write(STDOUT_FILENO, "\n\033[K", 4); rows++;
        if (is_sel) {
            write(STDOUT_FILENO, "\033[1;32m▶ \033[0m", strlen("\033[1;32m▶ \033[0m"));
            snprintf(idx_str, sizeof(idx_str), "\033[1;32m%2d\033[0m  ", abs_index);
        } else {
            write(STDOUT_FILENO, "  ", 2);
            snprintf(idx_str, sizeof(idx_str), "\033[2;37m%2d\033[0m  ", abs_index);
        }
        write(STDOUT_FILENO, idx_str, strlen(idx_str));
        if (is_sel) write(STDOUT_FILENO, "\033[1m", 4);

        /* write entry — replace \n with \n\033[K for clean lines */
        const char *p = entries[i];
        while (*p) {
            if (*p == '\n') {
                write(STDOUT_FILENO, "\n\033[K    ", 8); /* indent continuation */
                rows++;
            } else {
                write(STDOUT_FILENO, p, 1);
            }
            p++;
        }
        if (is_sel) write(STDOUT_FILENO, "\033[0m", 4);
        free(entries[i]);
    }

    char esc[16];
    snprintf(esc, sizeof(esc), "\033[%dA", rows);
    write(STDOUT_FILENO, esc, strlen(esc));
    *panel_rows = rows;
}

static void panel_free(char **items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) free(items[i]);
    free(items);
}

static void panel_rebuild(const char *buf, int len, int pos,
                           char ***items_out, int *count_out) {
    if (!g_config.panel_enabled) { *items_out = NULL; *count_out = 0; return; }
    *items_out = NULL; *count_out = 0;
    if (len == 0) return;

    int first_space = -1;
    for (int i = 0; i < len; i++) if (buf[i] == ' ') { first_space = i; break; }

    char **items = malloc(64 * sizeof(char *));
    if (!items) return;
    int count = 0, cap = 64;

    if (first_space == -1) {
        char word[MAXIMUM_INPUT];
        strncpy(word, buf, len); word[len] = '\0';
        int wlen = len;
        const char *builtins[] = {"cd","exit","export","pwd","echo","alias","unalias","source","jobs","fg","bg",NULL};
        for (int i = 0; builtins[i]; i++)
            if (strncmp(builtins[i], word, wlen) == 0) { if (count < 60) items[count++] = strdup(builtins[i]); }
        AliasCbCtx actx = { items, &count, word, wlen, 60 };
        alias_each(alias_collect_cb, &actx);
        char *path_env = getenv("PATH");
        if (path_env) {
            char path_copy[4096]; strncpy(path_copy, path_env, sizeof(path_copy)-1);
            char *dir = strtok(path_copy, ":");
            while (dir && count < 60) {
                DIR *d = opendir(dir); if (!d) { dir = strtok(NULL, ":"); continue; }
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL && count < 60) {
                    if (strncmp(ent->d_name, word, wlen) != 0) continue;
                    /* binary check — only show if actually executable */
                    char full_path[2048];
                    snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);
                    if (access(full_path, X_OK) != 0) continue;
                    int dup = 0;
                    for (int j = 0; j < count; j++) if (strcmp(items[j], ent->d_name) == 0) { dup=1; break; }
                    if (!dup) items[count++] = strdup(ent->d_name);
                }
                closedir(d); dir = strtok(NULL, ":");
            }
        }
    } else {
        char cmd[256] = {0}; strncpy(cmd, buf, first_space < 255 ? first_space : 255);
        if (strcmp(cmd, "cd") == 0) {
            int ws2 = pos;
            while (ws2 > 0 && buf[ws2-1] != ' ') ws2--;
            int wlen2 = pos - ws2;
            char word2[MAXIMUM_INPUT] = {0};
            if (wlen2 > 0) strncpy(word2, buf + ws2, wlen2);

            int total_cap = 64;
            char **merged = malloc(total_cap * sizeof(char *));
            if (!merged) { *items_out = NULL; *count_out = 0; return; }
            int merged_count = 0;

            {
                extern char **cd_frecency_list(const char *query,
                                               int limit, int *count_out);
                int fcount = 0;
                char **flist = cd_frecency_list(
                    wlen2 > 0 ? word2 : "", 8, &fcount);

                if (flist && fcount > 0) {
                    /* separator */
                    if (merged_count < total_cap)
                        merged[merged_count++] = strdup("★ frecency");

                    for (int fi = 0; fi < fcount &&
                                     merged_count < total_cap; fi++) {
                        struct stat st;
                        if (stat(flist[fi], &st) == 0 &&
                            S_ISDIR(st.st_mode)) {
                            char display[512];
                            const char *home = getenv("HOME");
                            if (home && strncmp(flist[fi], home,
                                                strlen(home)) == 0) {
                                snprintf(display, sizeof(display), "~%s",
                                         flist[fi] + strlen(home));
                            } else {
                                strncpy(display, flist[fi],
                                        sizeof(display)-1);
                            }
                            merged[merged_count++] = strdup(display);
                        }
                        free(flist[fi]);
                    }
                    free(flist);
                }
            }

            {
                char pattern[MAXIMUM_INPUT];
                if (wlen2 == 0) {
                    strncpy(pattern, "./*", MAXIMUM_INPUT-1);
                } else {
                    /* `cd foo<TAB>` → foo* glob */
                    strncpy(pattern, word2, MAXIMUM_INPUT-2);
                    strncat(pattern, "*",
                            MAXIMUM_INPUT - strlen(pattern) - 1);
                }

                /* separator */
                if (merged_count < total_cap)
                    merged[merged_count++] = strdup("📁 subdirs");

                glob_t g;
                if (glob(pattern, GLOB_MARK | GLOB_TILDE, NULL, &g) == 0) {
                    for (size_t gi = 0;
                         gi < g.gl_pathc && merged_count < total_cap;
                         gi++) {
                        const char *entry = g.gl_pathv[gi];
                        int elen = strlen(entry);
                        if (elen == 0 || entry[elen-1] != '/') continue;
                        merged[merged_count++] = strdup(entry);
                    }
                    globfree(&g);
                }
            }

            if (merged_count == 0) { free(merged); return; }
            *items_out  = merged;
            *count_out  = merged_count;
            return;
        }

        int ws = pos; while (ws > 0 && buf[ws-1] != ' ') ws--;
        int wlen = pos - ws; char word[MAXIMUM_INPUT] = {0}; strncpy(word, buf + ws, wlen);
        char cmdline[MAXIMUM_INPUT] = {0}; strncpy(cmdline, buf, len); cmdline[len] = '\0';
        int dyn_count = 0;
        char **dyn = get_dynamic_completions(cmdline, pos, &dyn_count);
        if (dyn) {
            for (int i = 0; i < dyn_count && count < 60; i++) items[count++] = dyn[i];
            free(dyn); *items_out = items; *count_out = count; return;
        }
        int sub_count = 0;
        char **subcmds = get_subcommands(cmd, word, &sub_count);
        if (subcmds) {
            for (int i = 0; i < sub_count && count < 60; i++) items[count++] = subcmds[i];
            free(subcmds); *items_out = items; *count_out = count; return;
        }
        char pattern[MAXIMUM_INPUT]; int cd_mode = (strcmp(cmd, "cd") == 0);
        if (wlen == 0) strncpy(pattern, "./*", MAXIMUM_INPUT-1);
        else { strncpy(pattern, word, MAXIMUM_INPUT-2); strncat(pattern, "*", MAXIMUM_INPUT-strlen(pattern)-1); }
        glob_t g;
        if (glob(pattern, GLOB_MARK|GLOB_TILDE, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc && count < 60; i++) {
                const char *entry = g.gl_pathv[i];
                if (cd_mode) { int elen=strlen(entry); if (elen==0||entry[elen-1]!='/') continue; }
                items[count++] = strdup(entry);
            }
            globfree(&g);
        }
    }
    if (count == 0) { free(items); return; }
    *items_out = items; *count_out = count;
}

static void panel_render(char **items, int count, int sel, int *panel_rows_out) {
    if (*panel_rows_out > 0) {
        write(STDOUT_FILENO, "\r", 1);
        for (int i = 0; i < *panel_rows_out; i++) { write(STDOUT_FILENO, "\033[B", 3); write(STDOUT_FILENO, "\033[K", 3); }
        char esc_clear[16]; snprintf(esc_clear, sizeof(esc_clear), "\033[%dA", *panel_rows_out);
        write(STDOUT_FILENO, esc_clear, strlen(esc_clear)); *panel_rows_out = 0;
    }
    if (!items || count == 0) return;
    int term_width = 80;
    struct winsize ws; if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) term_width = ws.ws_col;
    int col_width = 0;
    for (int i = 0; i < count; i++) { int l = utf8_display_len(items[i]); if (l > col_width) col_width = l; }
    col_width += 2;
    int cols = term_width / col_width; if (cols < 1) cols = 1;
    int max_rows = 4, visible = count < cols * max_rows ? count : cols * max_rows;
    int rows = 0;
    for (int i = 0; i < visible; i++) {
        if (i % cols == 0) { write(STDOUT_FILENO, "\n\033[K", 4); rows++; }
        if (items[i][0] == 0x2605 ||   /* ★ (UTF-8: E2 98 85) */
    (items[i][0] == (char)0xE2 && /* UTF-8 ★ */
     (unsigned char)items[i][1] == 0x98 &&
     (unsigned char)items[i][2] == 0x85) ||
    (items[i][0] == (char)0xF0 && /* UTF-8 📁 */
     (unsigned char)items[i][1] == 0x9F)) {
            write(STDOUT_FILENO, "\033[2;33m", 7);
            write(STDOUT_FILENO, items[i], strlen(items[i]));
            write(STDOUT_FILENO, "\033[0m", 4);
            int pad = col_width - utf8_display_len(items[i]);
            for (int pp = 0; pp < pad; pp++)
                write(STDOUT_FILENO, " ", 1);
            continue;
     }
        write(STDOUT_FILENO, i == sel ? "\033[7m" : "\033[2;36m", i == sel ? 4 : 7);
        write(STDOUT_FILENO, items[i], strlen(items[i]));
        write(STDOUT_FILENO, "\033[0m", 4);
        int pad = col_width - utf8_display_len(items[i]);
        for (int p = 0; p < pad; p++) write(STDOUT_FILENO, " ", 1);
    }
    char esc[16]; snprintf(esc, sizeof(esc), "\033[%dA", rows);
    write(STDOUT_FILENO, esc, strlen(esc)); *panel_rows_out = rows;
}

static void ctr_clear_list(int *list_rows) {
    if (*list_rows == 0) return;
    /* move down past all list rows, erase to end of screen, come back up */
    for (int i = 0; i < *list_rows; i++)
        write(STDOUT_FILENO, "\033[B", 3);
    write(STDOUT_FILENO, "\033[J", 3);
    char esc[16];
    snprintf(esc, sizeof(esc), "\033[%dA", *list_rows);
    write(STDOUT_FILENO, esc, strlen(esc));
    *list_rows = 0;
}

static void write_highlighted(const char *str, const char *query, int qlen) {
    if (!query || qlen == 0) { write(STDOUT_FILENO, str, strlen(str)); return; }
    const char *p = str;
    while (*p) {
        int match = 1;
        for (int i = 0; i < qlen; i++) {
            if (!p[i] || tolower((unsigned char)p[i]) != tolower((unsigned char)query[i])) { match=0; break; }
        }
        if (match) { write(STDOUT_FILENO, "\033[1;33m", 7); write(STDOUT_FILENO, p, qlen); write(STDOUT_FILENO, "\033[0m", 4); p += qlen; }
        else { write(STDOUT_FILENO, p, 1); p++; }
    }
}

static void ctr_render_prompt(const char *query, int qlen, char **results, int rcount, int sel) {
    write(STDOUT_FILENO, "\r\033[K", 4);
    write(STDOUT_FILENO, "\033[2;37m(search) \033[0m", strlen("\033[2;37m(search) \033[0m"));
    if (results && rcount > 0 && sel < rcount) {
        /* show only first line of multiline results in the prompt */
        const char *res = results[sel];
        const char *nl = strchr(res, '\n');
        if (nl) {
            /* write up to first newline, then ellipsis */
            write(STDOUT_FILENO, res, nl - res);
            write(STDOUT_FILENO, "\033[2;37m…\033[0m", strlen("\033[2;37m…\033[0m"));
        } else {
            write_highlighted(res, query, qlen);
        }
    }
    write(STDOUT_FILENO, "  \033[2;37m>\033[0m  ", 16);
    write(STDOUT_FILENO, "\033[1;36m", 7); write(STDOUT_FILENO, query, qlen); write(STDOUT_FILENO, "\033[0m\033[K", 7);
}

static void ctr_render_list(char **results, int *ids, int rcount, int sel, int *list_rows, const char *query, int qlen) {
    ctr_clear_list(list_rows);
    if (!results || rcount == 0) return;
    int show = rcount < 8 ? rcount : 8;
    for (int i = 0; i < show; i++) {
        write(STDOUT_FILENO, "\n\033[K", 4); (*list_rows)++;
        char idx[32];
        if (i == sel) snprintf(idx,sizeof(idx),"\033[1;32m▶ %2d\033[0m  \033[1m",ids[i]);
        else          snprintf(idx,sizeof(idx),"  \033[2;37m%2d\033[0m  ",ids[i]);
        write(STDOUT_FILENO, idx, strlen(idx));
        /* write result — replace \n with space to keep single-line display */
        const char *p = results[i];
        while (*p) {
            if (*p == '\n') {
                write(STDOUT_FILENO, " ", 1);
            } else {
                /* highlight query match */
                int match = (qlen > 0);
                if (match) {
                    for (int qi = 0; qi < qlen; qi++) {
                        if (!p[qi] || tolower((unsigned char)p[qi]) !=
                                      tolower((unsigned char)query[qi]))
                            { match = 0; break; }
                    }
                }
                if (match) {
                    write(STDOUT_FILENO, "\033[1;33m", 7);
                    write(STDOUT_FILENO, p, qlen);
                    write(STDOUT_FILENO, "\033[0m", 4);
                    p += qlen;
                    continue;
                }
                write(STDOUT_FILENO, p, 1);
            }
            p++;
        }
        if (i == sel) write(STDOUT_FILENO, "\033[0m", 4);
    }
    char esc[16]; snprintf(esc, sizeof(esc), "\033[%dA", *list_rows);
    write(STDOUT_FILENO, esc, strlen(esc));
}

static char *search_history_interactive(const char *prompt_str) {
    (void)prompt_str;
    char query[MAXIMUM_INPUT] = {0};
    int qlen=0, sel=0, rcount=0, list_rows=0;
    char **results=NULL; int *result_ids=NULL;
#define FREE_CTR() do { if(results){for(int _i=0;_i<rcount;_i++)free(results[_i]);free(results);results=NULL;rcount=0;} if(result_ids){free(result_ids);result_ids=NULL;} } while(0)
    ctr_render_prompt(query,qlen,results,rcount,sel);
    ctr_render_list(results,result_ids,rcount,sel,&list_rows,query,qlen);
    ctr_render_prompt(query,qlen,results,rcount,sel);
    while(1) {
        char c; if(read(STDIN_FILENO,&c,1)<=0){FREE_CTR();return NULL;}

        /* Enter — accept */
        if(c=='\r'||c=='\n'){
            char *ret=(results&&rcount>0&&sel<rcount)?strdup(results[sel]):NULL;
            ctr_clear_list(&list_rows);
            write(STDOUT_FILENO,"\r\033[K\r\n",6);
            FREE_CTR(); return ret;
        }

        /* Ctrl+C — cancel */
        if(c==3){
            ctr_clear_list(&list_rows);
            write(STDOUT_FILENO,"\r\033[K\r\n",6);
            FREE_CTR(); return NULL;
        }

        /* ESC sequence — arrow keys */
        if(c==27){
            /* read next two bytes blocking — we are already in raw mode */
            char b1=0, b2=0;
            if(read(STDIN_FILENO,&b1,1)<=0){ FREE_CTR(); return NULL; }
            if(b1=='[') {
                if(read(STDIN_FILENO,&b2,1)<=0){ FREE_CTR(); return NULL; }
                if(b2=='A') { /* up */
                    if(rcount>0) sel=(sel-1+rcount)%rcount;
                    ctr_render_prompt(query,qlen,results,rcount,sel);
                    ctr_render_list(results,result_ids,rcount,sel,&list_rows,query,qlen);
                    ctr_render_prompt(query,qlen,results,rcount,sel);
                } else if(b2=='B') { /* down */
                    if(rcount>0) sel=(sel+1)%rcount;
                    ctr_render_prompt(query,qlen,results,rcount,sel);
                    ctr_render_list(results,result_ids,rcount,sel,&list_rows,query,qlen);
                    ctr_render_prompt(query,qlen,results,rcount,sel);
                }
                /* other sequences: ignore */
            } else {
                /* plain ESC — cancel */
                ctr_clear_list(&list_rows);
                write(STDOUT_FILENO,"\r\033[K\r\n",6);
                FREE_CTR(); return NULL;
            }
            continue;
        }

        /* Ctrl+R — next result */
        if(c==18){if(rcount>0)sel=(sel+1)%rcount;ctr_render_prompt(query,qlen,results,rcount,sel);ctr_render_list(results,result_ids,rcount,sel,&list_rows,query,qlen);ctr_render_prompt(query,qlen,results,rcount,sel);continue;}

        /* Backspace */
        if(c==127||c==8){if(qlen>0){qlen--;query[qlen]='\0';sel=0;FREE_CTR();if(qlen>0)results=history_search_multi(query,8,&rcount,&result_ids);}ctr_render_prompt(query,qlen,results,rcount,sel);ctr_render_list(results,result_ids,rcount,sel,&list_rows,query,qlen);ctr_render_prompt(query,qlen,results,rcount,sel);continue;}

        /* Printable */
        if(c>=32&&c<127){if(qlen<MAXIMUM_INPUT-1){query[qlen++]=c;query[qlen]='\0';sel=0;FREE_CTR();results=history_search_multi(query,8,&rcount,&result_ids);}ctr_render_prompt(query,qlen,results,rcount,sel);ctr_render_list(results,result_ids,rcount,sel,&list_rows,query,qlen);ctr_render_prompt(query,qlen,results,rcount,sel);continue;}
    }
#undef FREE_CTR
}

static int common_prefix_len(glob_t *g) {
    if (g->gl_pathc == 0) return 0;
    int len = strlen(g->gl_pathv[0]);
    for (size_t i = 1; i < g->gl_pathc; i++) {
        int j = 0;
        while (j < len && g->gl_pathv[0][j] == g->gl_pathv[i][j]) j++;
        len = j;
    }
    return len;
}

/* ------------------------------------------------------------------ */
/*  read_line                                                           */
/*                                                                      */
/*  KEY CHANGES vs original:                                            */
/*  1. buf is heap-allocated (supports multi-line blocks)               */
/*  2. ml_prev_rows tracks screen rows for redraw                       */
/*  3. Enter checks ml_count_depth — inserts '\n' if block still open  */
/*  4. render_ml() used instead of render_with_suggestion()            */
/*  Everything else is identical to the original.                       */
/* ------------------------------------------------------------------ */

char *read_line(const char *prompt) {
    struct termios orig_termios, raw;
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return NULL;
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
#ifdef IUTF8
    raw.c_iflag |= IUTF8;
#endif
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return NULL;

    /* Use heap buffer to support multi-line blocks */
    int buf_cap = MAXIMUM_INPUT * 4;
    char *buf = calloc(1, buf_cap);
    if (!buf) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); return NULL; }

    int len = 0, pos = 0, hist_off = 0;
    int history_total = history_count();
    char saved[MAXIMUM_INPUT] = {0};

    int panel_sel = -1, panel_rows = 0;
    char **panel_items = NULL; int panel_count = 0;

    /* multiline: how many screen rows drawn last render */
    int ml_prev_rows = 1;

    panel_render(panel_items, panel_count, panel_sel, &panel_rows);
    ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
            free(buf); return NULL;
        }

        /* TAB */
        if (c == '\t') {
            char *sug = find_suggestion(buf, len);
            if (sug) {
                int sug_len = strlen(sug);
                if (sug_len > len && sug_len < buf_cap - 1) {
                    strncpy(buf, sug, buf_cap - 1); len = sug_len; pos = len;
                    free(sug);
                    panel_free(panel_items, panel_count);
                    panel_rebuild(buf, len, pos, &panel_items, &panel_count);
                    panel_sel = -1;
                    panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                    ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                    continue;
                }
                free(sug);
            }
            if (panel_count == 0) { write(STDOUT_FILENO, "\a", 1); continue; }
            if (panel_count == 1 && panel_sel == -1) {
                int ws = pos; while (ws > 0 && buf[ws-1] != ' ') ws--;
                const char *match = panel_items[0]; int match_len = strlen(match); int tail = len - pos;
                memmove(buf + ws + match_len, buf + pos, tail);
                memcpy(buf + ws, match, match_len);
                len = ws + match_len + tail; pos = ws + match_len; buf[len] = '\0';
                panel_free(panel_items, panel_count);
                panel_rebuild(buf, len, pos, &panel_items, &panel_count);
                panel_sel = -1;
                panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                continue;
            }
            int visible_count = panel_count < 60 ? panel_count : 60;
            {
                int term_width = 80;
                struct winsize ws_tmp;
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws_tmp) == 0 && ws_tmp.ws_col > 0)
                    term_width = ws_tmp.ws_col;
                int col_width = 0;
                for (int i = 0; i < panel_count; i++) {
                    int l = utf8_display_len(panel_items[i]);
                    if (l > col_width) col_width = l;
                }
                col_width += 2;
                int cols = term_width / col_width;
                if (cols < 1) cols = 1;
                visible_count = panel_count < cols * 4 ? panel_count : cols * 4;
            }
            // debug panel_sel = (panel_sel == -1) ? 0 : (panel_sel + 1) % visible_count;
            do {
                panel_sel = (panel_sel == -1) ? 0
                          : (panel_sel + 1) % visible_count;
            } while (panel_sel < panel_count &&
                     panel_items[panel_sel] != NULL &&
                     (panel_items[panel_sel][0] == (char)0xE2 ||   /* ★ */
                      panel_items[panel_sel][0] == (char)0xF0));   /* 📁 */

            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
            continue;
        }

        /* Ctrl+D */
        if (c == 4) {
            if (len == 0) {
                panel_free(panel_items, panel_count);
                panel_render(NULL, 0, -1, &panel_rows);
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
                free(buf); return NULL;
            }
            continue;
        }

        /* Ctrl+C */
        if (c == 3) {
            extern volatile int g_interrupt_loop;
            g_interrupt_loop = 1;

            if (len > 0) {
                write(STDOUT_FILENO, "^C\r\n", 4);
            }

            memset(buf, 0, buf_cap);
            len = pos = hist_off = 0;
            memset(saved, 0, sizeof(saved));
            panel_free(panel_items, panel_count);
            panel_items = NULL; panel_count = 0; panel_sel = -1;
            panel_render(NULL, 0, -1, &panel_rows);
            ml_prev_rows = 1;
            g_interrupt_loop = 0;
            ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
            continue;
        }



        /* Enter */
        if (c == '\r' || c == '\n') {
            /* panel item selected — insert it */
            if (panel_sel >= 0 && panel_items) {
                int ws = pos; while (ws > 0 && buf[ws-1] != ' ') ws--;
                const char *match = panel_items[panel_sel]; int mlen = strlen(match); int tail = len - pos;
                memmove(buf + ws + mlen, buf + pos, tail);
                memcpy(buf + ws, match, mlen);
                len = ws + mlen + tail; pos = ws + mlen; buf[len] = '\0';
                panel_free(panel_items, panel_count);
                panel_rebuild(buf, len, pos, &panel_items, &panel_count);
                panel_sel = -1;
                panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                continue;
            }

            /* ── MULTILINE: check if block is still open ── */
            int depth = ml_count_depth(buf, len);
            if (depth > 0) {
                /* insert newline at cursor */
                if (len + 1 < buf_cap - 1) {
                    memmove(buf + pos + 1, buf + pos, len - pos);
                    buf[pos] = '\n'; len++; pos++; buf[len] = '\0';
                }
                panel_free(panel_items, panel_count); panel_items = NULL; panel_count = 0; panel_sel = -1;
                panel_render(NULL, 0, -1, &panel_rows);
                ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                continue;
            }

            /* commit — print final rendered state then newline */
            /* re-render to ensure display is clean */
            if (panel_rows > 0) {
                char esc[16];
                snprintf(esc, sizeof(esc), "\033[%dB", panel_rows);
                write(STDOUT_FILENO, esc, strlen(esc));
            }
            /* erase everything below cursor (clears panel rows) */
            write(STDOUT_FILENO, "\033[J", 3);
            /* go back up to input line */
            if (panel_rows > 0) {
                char esc[16];
                snprintf(esc, sizeof(esc), "\033[%dA", panel_rows);
                write(STDOUT_FILENO, esc, strlen(esc));
            }
            panel_rows = 0;
            ml_render(prompt, buf, len, len, ml_prev_rows);
            write(STDOUT_FILENO, "\033[K\r\n", 5);

            panel_free(panel_items, panel_count);
            panel_items = NULL; panel_count = 0; panel_sel = -1;
            history_total++;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
            tcflush(STDIN_FILENO, TCIFLUSH);
            if (len > 0) history_add(buf);
            char *result = strdup(buf);
            free(buf);
            return result;
        }

        /* Backspace */
        if (c == 127 || c == 8) {
            if (pos > 0) {
                /* allow deleting '\n' (merges lines) */
                int back = (buf[pos-1] == '\n') ? 1 : utf8_prev_char_len(buf, pos);
                memmove(buf + pos - back, buf + pos, len - pos);
                len -= back; pos -= back; buf[len] = '\0';
                if (len == 0) { hist_off = 0; memset(saved, 0, sizeof(saved)); }
            }
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
            continue;
        }

        /* Ctrl+A */
        if (c == 1) {
            while (pos > 0 && buf[pos-1] != '\n') pos--;
            if (panel_sel >= 0) { panel_sel = -1; panel_render(panel_items, panel_count, panel_sel, &panel_rows); }
            ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
            continue;
        }

        /* Ctrl+E */
        if (c == 5) {
            while (pos < len && buf[pos] != '\n') pos++;
            if (panel_sel >= 0) { panel_sel = -1; panel_render(panel_items, panel_count, panel_sel, &panel_rows); }
            ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
            continue;
        }

        /* Ctrl+F */
        if (c == 6) {
            if (pos < len) { pos += utf8_char_len(buf, pos); if (pos > len) pos = len; }
            if (panel_sel >= 0) { panel_sel = -1; panel_render(panel_items, panel_count, panel_sel, &panel_rows); }
            ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
            continue;
        }

        /* Ctrl+B */
        if (c == 2) {
            if (pos > 0) { pos -= utf8_prev_char_len(buf, pos); if (pos < 0) pos = 0; }
            if (panel_sel >= 0) { panel_sel = -1; panel_render(panel_items, panel_count, panel_sel, &panel_rows); }
            ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
            continue;
        }

        /* Ctrl+R */
        if (c == 18) {
            char *result = search_history_interactive(prompt);
            write(STDOUT_FILENO, "\r\033[J", 4);
            if (result) {
                strncpy(buf, result, buf_cap - 1); buf[buf_cap-1] = '\0';
                len = strlen(buf); pos = len; free(result);
            } else {
                memset(buf, 0, buf_cap); len = pos = 0;
            }
            hist_off = 0; ml_prev_rows = 1;
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
            continue;
        }

        /* ESC sequence */
        if (c == 27) {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;

            if (seq[0] == '[') {
                /* Up arrow */
                if (seq[1] == 'A') {
                    if (panel_sel >= 0) {
                        panel_sel--;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                        continue;
                    }
                    /*
                     * If already in history mode (hist_off > 0), always go
                     * further back in history — never do logical line movement
                     * on a history entry.
                     * If not in history mode, only do logical line movement
                     * when cursor is NOT on the first logical line.
                     */
                    int on_first = 1;
                    for (int i = 0; i < pos; i++) if (buf[i] == '\n') { on_first = 0; break; }
                    if (!on_first && hist_off == 0) {
                        /* move cursor to previous logical line, same column */
                        int col = 0;
                        for (int i = pos-1; i >= 0 && buf[i] != '\n'; i--) col++;
                        int prev_nl = pos - col - 1;
                        int prev_start = prev_nl - 1;
                        while (prev_start > 0 && buf[prev_start-1] != '\n') prev_start--;
                        int prev_len = prev_nl - prev_start;
                        pos = prev_start + (col < prev_len ? col : prev_len);
                        ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                        continue;
                    }
                    /* history up */
                    if (hist_off == 0) strncpy(saved, buf, sizeof(saved)-1);
                    hist_off++;
                    char *h = history_get(hist_off);
                    if (h) {
                        strncpy(buf, h, buf_cap-1); buf[buf_cap-1] = '\0';
                        len = strlen(buf); pos = len; free(h);
                    } else hist_off--;
                    clear_input_area(&ml_prev_rows, &panel_rows);
                    panel_show_history(hist_off, &panel_rows);
                    ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                    continue;
                }

                /* Down arrow */
                if (seq[1] == 'B') {
                    if (panel_sel == -1 && panel_count > 0 && hist_off == 0) {
                        panel_sel = 0;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                        continue;
                    } else if (panel_sel >= 0) {
                        panel_sel = (panel_sel + 1) % panel_count;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                        continue;
                    }
                    /* if on last logical line: history down; else move down.
                     * In history mode, always navigate history (never logical lines). */
                    int on_last = 1;
                    for (int i = pos; i < len; i++) if (buf[i] == '\n') { on_last = 0; break; }
                    if (!on_last && hist_off == 0) {
                        int col = 0;
                        for (int i = pos-1; i >= 0 && buf[i] != '\n'; i--) col++;
                        int next_nl = pos;
                        while (next_nl < len && buf[next_nl] != '\n') next_nl++;
                        int next_start = next_nl + 1;
                        int next_end = next_start;
                        while (next_end < len && buf[next_end] != '\n') next_end++;
                        int next_len = next_end - next_start;
                        pos = next_start + (col < next_len ? col : next_len);
                        if (pos > len) pos = len;
                        ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                        continue;
                    }
                    if (hist_off == 0) continue;
                    hist_off--;
                    if (hist_off == 0) {
                        strncpy(buf, saved, buf_cap-1); buf[buf_cap-1] = '\0';
                        len = strlen(buf); pos = len;
                    } else {
                        char *h = history_get(hist_off);
                        if (h) { strncpy(buf, h, buf_cap-1); buf[buf_cap-1] = '\0'; len = strlen(buf); pos = len; free(h); }
                    }
                    clear_input_area(&ml_prev_rows, &panel_rows);
                    if (hist_off > 0) {
                        panel_show_history(hist_off, &panel_rows);
                    } else {
                        panel_free(panel_items, panel_count);
                        panel_rebuild(buf, len, pos, &panel_items, &panel_count);
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                    }
                    ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                    continue;
                }

                /* Right arrow */
                if (seq[1] == 'C') {
                    if (panel_sel >= 0) {
                        int term_width = 80; struct winsize ws2;
                        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws2) == 0 && ws2.ws_col > 0) term_width = ws2.ws_col;
                        int col_width = 0;
                        for (int i = 0; i < panel_count; i++) { int l = utf8_display_len(panel_items[i]); if (l > col_width) col_width = l; }
                        col_width += 2; int cols = term_width / col_width; if (cols < 1) cols = 1;
                        panel_sel = (panel_sel + 1) % panel_count;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                        continue;
                    }
                    if (pos < len) { pos += utf8_char_len(buf, pos); if (pos > len) pos = len; }
                    ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                    continue;
                }

                /* Left arrow */
                if (seq[1] == 'D') {
                    if (panel_sel >= 0) {
                        int term_width = 80; struct winsize ws2;
                        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws2) == 0 && ws2.ws_col > 0) term_width = ws2.ws_col;
                        int col_width = 0;
                        for (int i = 0; i < panel_count; i++) { int l = utf8_display_len(panel_items[i]); if (l > col_width) col_width = l; }
                        col_width += 2; int cols = term_width / col_width; if (cols < 1) cols = 1;
                        panel_sel = (panel_sel - 1 + panel_count) % panel_count;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                        continue;
                    }
                    if (pos > 0) { pos -= utf8_prev_char_len(buf, pos); if (pos < 0) pos = 0; }
                    ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
                    continue;
                }
            } else {
                if (panel_sel >= 0) { panel_sel = -1; panel_render(panel_items, panel_count, panel_sel, &panel_rows); ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows); }
            }
            continue;
        }

        /* ASCII printable */
        if ((unsigned char)c >= 32 && c < 127) {
            if (panel_sel >= 0) panel_sel = -1;
            if (len + 1 < buf_cap - 1) {
                memmove(buf + pos + 1, buf + pos, len - pos);
                buf[pos] = c; len++; pos++; buf[len] = '\0';
            }
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
            continue;
        }

        /* UTF-8 multi-byte */
        if ((unsigned char)c >= 0xC0 && (unsigned char)c <= 0xF7) {
            int nb; unsigned char uc = (unsigned char)c;
            if (uc >= 0xF0) nb = 4; else if (uc >= 0xE0) nb = 3; else nb = 2;
            if (len + nb < buf_cap - 1) {
                char seq[4]; seq[0] = c;
                for (int b = 1; b < nb; b++) {
                    if (read(STDIN_FILENO, &seq[b], 1) <= 0) goto done;
                    if (((unsigned char)seq[b] & 0xC0) != 0x80) goto done;
                }
                memmove(buf + pos + nb, buf + pos, len - pos);
                memcpy(buf + pos, seq, nb); len += nb; pos += nb; buf[len] = '\0';
            }
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            ml_prev_rows = render_ml(prompt, buf, len, pos, ml_prev_rows);
            continue;
        }
    }

done:;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    free(buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  read_heredoc (unchanged)                                            */
/* ------------------------------------------------------------------ */

char *read_heredoc(const char *delimiter, int expand) {
    (void)expand;
    size_t buf_cap = 4096; char *buf = malloc(buf_cap);
    if (!buf) return NULL; buf[0] = '\0'; size_t buf_len = 0;
    char line[MAXIMUM_INPUT]; int delim_len = strlen(delimiter);
    write(STDOUT_FILENO, "> ", 2);
    while (1) {
        if (!fgets(line, sizeof(line), stdin)) break;
        int line_len = strlen(line);
        if (line_len > 0 && line[line_len-1] == '\n') { line[line_len-1] = '\0'; line_len--; }
        if (line_len == delim_len && strcmp(line, delimiter) == 0) break;
        write(STDOUT_FILENO, "> ", 2);
        size_t needed = buf_len + line_len + 2;
        if (needed > buf_cap) { while (buf_cap < needed) buf_cap *= 2; char *tmp = realloc(buf, buf_cap); if (!tmp) { free(buf); return NULL; } buf = tmp; }
        memcpy(buf + buf_len, line, line_len); buf_len += line_len; buf[buf_len++] = '\n'; buf[buf_len] = '\0';
    }
    return buf;
}