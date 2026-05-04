//
// Created by mete on 27.04.2026.
//

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <glob.h>
#include <dirent.h>
#include <ctype.h>
#include "../include/input.h"
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

static int utf8_display_len(const char *s) {
    int cols = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            cols++; s++;
        } else if (c < 0xC0) {
            /* continuation byte — skip, don't count */
            s++;
        } else if (c < 0xE0) {
            cols++; s += 2;
        } else if (c < 0xF0) {
            cols++; s += 3;
        } else {
            cols++; s += 4;
        }
    }
    return cols;
}

static void render(const char *prompt, const char *buf, int len, int pos) {
    write(STDOUT_FILENO, "\r", 1);
    write(STDOUT_FILENO, prompt, strlen(prompt));

    /* syntax highlighted buf */
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

    /* cursor geri al */
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

static int utf8_char_len(const char *buf, int pos) {
    unsigned char c = (unsigned char)buf[pos];
    if (c < 0x80) return 1;
    if (c < 0xC0) return 1; /* continuation byte — shouldn't happen at start */
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    return 4;
}

static int utf8_prev_char_len(const char *buf, int pos) {
    if (pos <= 0) return 0;
    int back = 1;
    /* walk back over continuation bytes 10xxxxxx */
    while (back < pos && back < 4 &&
           ((unsigned char)buf[pos - back] & 0xC0) == 0x80) {
        back++;
    }
    return back;
}

static int word_start(const char *buf, int pos) {
    int i = pos - 1;
    while (i > 0 && buf[i-1] != ' ') i--;
    return i;
}

/* Clears N lines below current line */
static void clear_below(int n) {
    for (int i = 0; i < n; i++) {
        write(STDOUT_FILENO, "\n\033[K", 4);
    }
    if (n > 0) {
        char esc[16];
        snprintf(esc, sizeof(esc), "\033[%dA", n);
        write(STDOUT_FILENO, esc, strlen(esc));
    }
}

typedef struct {
    char **items;
    int  *count;
    const char *word;
    int   wlen;
    int   cap;
} AliasCbCtx;

static void alias_collect_cb(const char *name, const char *value,
                              void *ud) {
    (void)value;
    AliasCbCtx *ctx = (AliasCbCtx *)ud;
    if (*ctx->count >= ctx->cap) return;
    if (strncmp(name, ctx->word, ctx->wlen) == 0) {
        /* dedup check */
        for (int j = 0; j < *ctx->count; j++) {
            if (strcmp(ctx->items[j], name) == 0) return;
        }
        ctx->items[(*ctx->count)++] = strdup(name);
    }
}



static void panel_show_history(int hist_off, int *panel_rows) {
    /* clear existing panel first */
    if (*panel_rows > 0) {
        for (int i = 0; i < *panel_rows; i++)
            write(STDOUT_FILENO, "\n\033[K", 4);
        char esc_clear[16];
        snprintf(esc_clear, sizeof(esc_clear), "\033[%dA", *panel_rows);
        write(STDOUT_FILENO, esc_clear, strlen(esc_clear));
        *panel_rows = 0;
    }

    char *entries[10];
    int count = 0;
    for (int i = 1; i <= 10; i++) {
        char *h = history_get(i);
        if (!h) break;
        entries[count++] = h;
    }
    if (count == 0) return;

    int total = history_total_count();
    int rows = 0;
    for (int i = 0; i < count; i++) {
        write(STDOUT_FILENO, "\n\033[K", 4);
        rows++;
        int is_selected = (i + 1 == hist_off);
        int abs_index = total - i;
        char index_str[16];
        if (is_selected) {
            const char *arrow = "\033[1;32m▶ \033[0m";
            write(STDOUT_FILENO, arrow, strlen(arrow));
            snprintf(index_str, sizeof(index_str), "\033[1;32m%2d\033[0m  ", abs_index);
        } else {
            write(STDOUT_FILENO, "  ", 2);
            snprintf(index_str, sizeof(index_str), "\033[2;37m%2d\033[0m  ", abs_index);
        }
        write(STDOUT_FILENO, index_str, strlen(index_str));
        if (is_selected) write(STDOUT_FILENO, "\033[1m", 4);
        write(STDOUT_FILENO, entries[i], strlen(entries[i]));
        if (is_selected) write(STDOUT_FILENO, "\033[0m", 4);
        free(entries[i]);
    }

    char esc[16];
    snprintf(esc, sizeof(esc), "\033[%dA", rows);
    write(STDOUT_FILENO, esc, strlen(esc));
    *panel_rows = rows;
}

/* Panel free helper */
static void panel_free(char **items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) free(items[i]);
    free(items);
}

/* Panel rebuild (call after every buf change) */
static void panel_rebuild(const char *buf, int len, int pos,
                          char ***items_out, int *count_out) {
    if (!g_config.panel_enabled) { *items_out=NULL; *count_out=0; return; }
    
    /* free old items first — caller handles this */
    *items_out = NULL;
    *count_out = 0;

    if (len == 0) return;

    /* find first space to determine if we are in first word */
    int first_space = -1;
    for (int i = 0; i < len; i++) {
        if (buf[i] == ' ') { first_space = i; break; }
    }

    char **items = malloc(64 * sizeof(char *));
    if (!items) return;
    int count = 0;
    int cap = 64;

    if (first_space == -1) {
        /* FIRST WORD — command completion */
        char word[MAXIMUM_INPUT];
        strncpy(word, buf, len);
        word[len] = '\0';
        int wlen = len;

        /* 1. builtins */
        const char *builtins[] = {
            "cd","exit","export","pwd","echo","alias","unalias",
            "source","jobs","fg","bg", NULL
        };
        for (int i = 0; builtins[i]; i++) {
            if (strncmp(builtins[i], word, wlen) == 0) {
                if (count < 60) items[count++] = strdup(builtins[i]);
            }
        }

        /* 2. aliases */
        AliasCbCtx actx = { items, &count, word, wlen, 60 };
        alias_each(alias_collect_cb, &actx);

        /* 3. PATH executables */
        char *path_env = getenv("PATH");
        if (path_env) {
            char path_copy[4096];
            strncpy(path_copy, path_env, sizeof(path_copy)-1);
            char *dir = strtok(path_copy, ":");
            while (dir && count < 60) {
                DIR *d = opendir(dir);
                if (!d) { dir = strtok(NULL, ":"); continue; }
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL && count < 60) {
                    if (strncmp(ent->d_name, word, wlen) != 0) continue;
                    /* dedup check */
                    int dup = 0;
                    for (int j = 0; j < count; j++) {
                        if (strcmp(items[j], ent->d_name) == 0) { dup=1; break; }
                    }
                    if (!dup) items[count++] = strdup(ent->d_name);
                }
                closedir(d);
                dir = strtok(NULL, ":");
            }
        }

    } else {
        /* ARGUMENT — file/dir completion */

        /* get command name */
        char cmd[256] = {0};
        strncpy(cmd, buf, first_space < 255 ? first_space : 255);

        /* get current word at cursor */
        int ws = pos;
        while (ws > 0 && buf[ws-1] != ' ') ws--;
        int wlen = pos - ws;
        char word[MAXIMUM_INPUT] = {0};
        strncpy(word, buf + ws, wlen);

        /* 0. try dynamic completion first (context-aware) */
        char cmdline[MAXIMUM_INPUT] = {0};
        strncpy(cmdline, buf, len);
        cmdline[len] = '\0';
        int dyn_count = 0;
        char **dyn = get_dynamic_completions(cmdline, pos, &dyn_count);
        if (dyn) {
            for (int i = 0; i < dyn_count && count < 60; i++)
                items[count++] = dyn[i];
            free(dyn);  /* free array, not strings */
            *items_out = items;
            *count_out = count;
            return;
        }

        /* 1. try subcommand completion first */
        int sub_count = 0;
        char **subcmds = get_subcommands(cmd, word, &sub_count);
        if (subcmds) {
            /* use subcommands as panel items */
            for (int i = 0; i < sub_count && count < 60; i++) {
                items[count++] = subcmds[i];  /* already malloc'd */
            }
            free(subcmds);  /* free array but not strings — moved to items */
            *items_out = items;
            *count_out = count;
            return;
        }

        /* 2. fall through to glob-based completion */
        /* build glob pattern */
        char pattern[MAXIMUM_INPUT];
        int cd_mode = (strcmp(cmd, "cd") == 0);

        if (wlen == 0) {
            strncpy(pattern, "./*", MAXIMUM_INPUT-1);
        } else {
            strncpy(pattern, word, MAXIMUM_INPUT-2);
            strncat(pattern, "*", MAXIMUM_INPUT-strlen(pattern)-1);
        }

        glob_t g;
        int r = glob(pattern, GLOB_MARK|GLOB_TILDE, NULL, &g);
        if (r == 0) {
            for (size_t i = 0; i < g.gl_pathc && count < 60; i++) {
                const char *entry = g.gl_pathv[i];
                /* cd mode: only dirs (ending in /) */
                if (cd_mode) {
                    int elen = strlen(entry);
                    if (elen == 0 || entry[elen-1] != '/') continue;
                }
                items[count++] = strdup(entry);
            }
            globfree(&g);
        }
    }

    if (count == 0) { free(items); return; }

    /* grow check not needed since we capped at 60 */
    *items_out = items;
    *count_out = count;
}

/* Panel render */
static void panel_render(char **items, int count, int sel,
                         int *panel_rows_out) {
    /* clear previous panel */
    if (*panel_rows_out > 0) {
        write(STDOUT_FILENO, "\r", 1);
        for (int i = 0; i < *panel_rows_out; i++) {
            write(STDOUT_FILENO, "\033[B", 3);
            write(STDOUT_FILENO, "\033[K", 3);
        }
        char esc_clear[16];
        snprintf(esc_clear, sizeof(esc_clear), "\033[%dA", *panel_rows_out);
        write(STDOUT_FILENO, esc_clear, strlen(esc_clear));
        *panel_rows_out = 0;
    }
    if (!items || count == 0) return;

    int term_width = 80;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        term_width = ws.ws_col;

    int col_width = 0;
    for (int i = 0; i < count; i++) {
        int l = utf8_display_len(items[i]);
        if (l > col_width) col_width = l;
    }
    col_width += 2;

    int cols = term_width / col_width;
    if (cols < 1) cols = 1;
    int max_rows = 4;
    int visible = count < cols * max_rows ? count : cols * max_rows;

    int rows = 0;
    for (int i = 0; i < visible; i++) {
        if (i % cols == 0) {
            write(STDOUT_FILENO, "\n\033[K", 4);
            rows++;
        }
        if (i == sel) {
            write(STDOUT_FILENO, "\033[7m", 4);
        } else {
            write(STDOUT_FILENO, "\033[2;36m", 7);
        }
        write(STDOUT_FILENO, items[i], strlen(items[i]));
        write(STDOUT_FILENO, "\033[0m", 4);
        int pad = col_width - utf8_display_len(items[i]);
        for (int p = 0; p < pad; p++) write(STDOUT_FILENO, " ", 1);
    }

    char esc[16];
    snprintf(esc, sizeof(esc), "\033[%dA", rows);
    write(STDOUT_FILENO, esc, strlen(esc));
    *panel_rows_out = rows;
}

static void ctr_clear_list(int *list_rows) {
    if (*list_rows == 0) return;
    /* save cursor */
    write(STDOUT_FILENO, "\033[s", 3);
    for (int i = 0; i < *list_rows; i++) {
        write(STDOUT_FILENO, "\n\033[K", 4);
    }
    /* restore cursor */
    write(STDOUT_FILENO, "\033[u", 3);
    *list_rows = 0;
}

static void write_highlighted(const char *str, const char *query, int qlen) {
    /* writes str to stdout, highlighting occurrences of query in bold yellow */
    if (!query || qlen == 0) {
        write(STDOUT_FILENO, str, strlen(str));
        return;
    }

    const char *p = str;
    while (*p) {
        /* case-insensitive search for query at position p */
        int match = 1;
        for (int i = 0; i < qlen; i++) {
            if (!p[i]) { match = 0; break; }
            if (tolower((unsigned char)p[i]) !=
                tolower((unsigned char)query[i])) {
                match = 0; break;
            }
        }
        if (match) {
            /* write matched part in bold yellow */
            write(STDOUT_FILENO, "\033[1;33m", 7);
            write(STDOUT_FILENO, p, qlen);
            write(STDOUT_FILENO, "\033[0m", 4);
            /* restore dim/bold state for selected/non-selected */
            p += qlen;
        } else {
            write(STDOUT_FILENO, p, 1);
            p++;
        }
    }
}
static int display_len_skip_ansi(const char *s) {
    int cols = 0;
    while (*s) {
        if (*s == '\033') {
            /* ANSI escape — ] */
            s++;
            if (*s == '[') {
                s++;
                while (*s && !isalpha((unsigned char)*s)) s++;
                if (*s) s++;
            }
            continue;
        }
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) { cols++; s++; }
        else if (c < 0xC0) { s++; }
        else if (c < 0xE0) { cols++; s += 2; }
        else if (c < 0xF0) { cols++; s += 3; }
        else { cols++; s += 4; }
    }
    return cols;
}

static void ctr_render_prompt(const char *query, int qlen,
                               char **results, int rcount, int sel) {
    write(STDOUT_FILENO, "\r\033[K", 4);

    const char *prefix = "(search) ";
    write(STDOUT_FILENO, "\033[2;37m", 7);
    write(STDOUT_FILENO, prefix, strlen(prefix));
    write(STDOUT_FILENO, "\033[0m", 4);

    if (results && rcount > 0 && sel < rcount) {
        write_highlighted(results[sel], query, qlen);
    }

    write(STDOUT_FILENO, "  \033[2;37m>\033[0m  ", 16);

    write(STDOUT_FILENO, "\033[1;36m", 7);
    write(STDOUT_FILENO, query, qlen);
    write(STDOUT_FILENO, "\033[0m", 4);

    write(STDOUT_FILENO, "\033[K", 3);

}

static void ctr_render_list(char **results, int *ids, int rcount,
                             int sel, int *list_rows,
                             const char *query, int qlen) {
    ctr_clear_list(list_rows);
    if (!results || rcount == 0) return;
    int show = rcount < 8 ? rcount : 8;
    for (int i = 0; i < show; i++) {
        write(STDOUT_FILENO, "\n\033[K", 4);
        (*list_rows)++;
        char idx[32];
        if (i == sel) {
            snprintf(idx, sizeof(idx),
                     "\033[1;32m▶ %2d\033[0m  \033[1m", ids[i]);
        } else {
            snprintf(idx, sizeof(idx),
                     "  \033[2;37m%2d\033[0m  ", ids[i]);
        }
        write(STDOUT_FILENO, idx, strlen(idx));
        write_highlighted(results[i], query, qlen);
        if (i == sel) write(STDOUT_FILENO, "\033[0m", 4);
    }
    /* move back up */
    char esc[16];
    snprintf(esc, sizeof(esc), "\033[%dA", *list_rows);
    write(STDOUT_FILENO, esc, strlen(esc));
}

static char *search_history_interactive(const char *prompt_str) {
    char query[MAXIMUM_INPUT] = {0};
    int  qlen = 0;
    int  sel = 0;          /* selected index in results list */
    char **results = NULL;
    int  *result_ids = NULL;
    int  rcount = 0;
    int  list_rows = 0;    /* how many rows the list occupies */

    /* free results helper */
    #define FREE_RESULTS() do { \
        if (results) { \
            for (int _i = 0; _i < rcount; _i++) free(results[_i]); \
            free(results); results = NULL; rcount = 0; \
        } \
        if (result_ids) { free(result_ids); result_ids = NULL; } \
    } while(0)

    ctr_render_prompt(query, qlen, results, rcount, sel);
    ctr_render_list(results, result_ids, rcount, sel, &list_rows, query, qlen);
    ctr_render_prompt(query, qlen, results, rcount, sel);

    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) {
            FREE_RESULTS();
            return NULL;
        }

        /* Enter — accept */
        if (c == '\r' || c == '\n') {
            write(STDOUT_FILENO, "\033[K", 3);
            char *ret = NULL;
            if (results && rcount > 0 && sel < rcount)
                ret = strdup(results[sel]);
            ctr_clear_list(&list_rows);
            write(STDOUT_FILENO, "\r", 1);
            write(STDOUT_FILENO, "\033[K", 3);   /* clear search prompt line */
            write(STDOUT_FILENO, "\r\n", 2);
            FREE_RESULTS();
            return ret;
        }

        /* ESC or Ctrl+C — cancel */
        if (c == 3) {
            ctr_clear_list(&list_rows);
            write(STDOUT_FILENO, "\r", 1);
            write(STDOUT_FILENO, "\033[K", 3);
            write(STDOUT_FILENO, "\r\n", 2);
            FREE_RESULTS();
            return NULL;
        }

        /* ESC sequence */
        if (c == 27) {
            struct termios nb;
            tcgetattr(STDIN_FILENO, &nb);
            nb.c_cc[VMIN] = 0;
            nb.c_cc[VTIME] = 1;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &nb);

            char seq[2] = {0, 0};
            int n1 = read(STDIN_FILENO, &seq[0], 1);

            nb.c_cc[VMIN] = 1;
            nb.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &nb);

            if (n1 <= 0 || seq[0] != '[') {
                /* plain ESC */
                ctr_clear_list(&list_rows);
                write(STDOUT_FILENO, "\r", 1);
                write(STDOUT_FILENO, "\033[K", 3);
                write(STDOUT_FILENO, "\r\n", 2);
                FREE_RESULTS();
                return NULL;
            }

            /* ESC [ sequence */
            read(STDIN_FILENO, &seq[1], 1);
            if (seq[1] == 'A' && sel > 0) sel--;
            if (seq[1] == 'B' && sel < rcount-1) sel++;
            ctr_render_prompt(query, qlen, results, rcount, sel);
            ctr_render_list(results, result_ids, rcount, sel,
                            &list_rows, query, qlen);
            ctr_render_prompt(query, qlen, results, rcount, sel);
            continue;
        }

        /* Ctrl+R — next match (cycle selection down) */
        if (c == 18) {
            if (rcount > 0) sel = (sel + 1) % rcount;
            ctr_render_prompt(query, qlen, results, rcount, sel);
            ctr_render_list(results, result_ids, rcount, sel, &list_rows, query, qlen);
            ctr_render_prompt(query, qlen, results, rcount, sel);
            continue;
        }

        /* Backspace */
        if (c == 127 || c == 8) {
            if (qlen > 0) {
                qlen--;
                query[qlen] = '\0';
                sel = 0;
                FREE_RESULTS();
                if (qlen > 0)
                    results = history_search_multi(query, 8, &rcount, &result_ids);
            }
            ctr_render_prompt(query, qlen, results, rcount, sel);
            ctr_render_list(results, result_ids, rcount, sel, &list_rows,query,qlen);
            ctr_render_prompt(query, qlen, results, rcount, sel);
            continue;
        }

        /* Printable char */
        if (c >= 32 && c < 127) {
            if (qlen < MAXIMUM_INPUT - 1) {
                query[qlen++] = c;
                query[qlen] = '\0';
                sel = 0;
                FREE_RESULTS();
                results = history_search_multi(query, 8, &rcount, &result_ids);
            }
            ctr_render_prompt(query, qlen, results, rcount, sel);
            ctr_render_list(results, result_ids, rcount, sel, &list_rows,query,qlen);
            ctr_render_prompt(query, qlen, results, rcount, sel);
            continue;
        }
    }
}

static char *find_suggestion(const char *buf, int len) {
    if (len == 0) return NULL;

    /* 1. Try history first — find most recent cmd starting with buf */
    /* Call history_search_prefix(buf) — we will add this to history.c */
    char *h = history_search_prefix(buf);
    if (h) return h;

    /* 2. Try filesystem — last word completion */
    int ws = word_start(buf, len);  /* reuse existing helper */
    int wlen = len - ws;
    if (wlen == 0) return NULL;

    char pattern[MAXIMUM_INPUT];
    strncpy(pattern, buf + ws, wlen);
    pattern[wlen] = '\0';
    strncat(pattern, "*", MAXIMUM_INPUT - wlen - 1);

    glob_t g;
    if (glob(pattern, GLOB_MARK|GLOB_TILDE, NULL, &g) == 0 && g.gl_pathc > 0) {
        char *result = strdup(g.gl_pathv[0]);
        globfree(&g);
        return result;  /* caller frees */
    }
    globfree(&g);
    return NULL;
}

static void render_with_suggestion(const char *prompt, const char *buf,
                                    int len, int pos) {
    if (!g_config.suggestion_enabled) return;
    
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

/* Returns the common prefix length of all matches */
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

/* Finds the start of the current word under/before cursor */


char *read_line(const char *prompt) {
    struct termios orig_termios, raw;

    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        return NULL;
    }

    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    #ifdef IUTF8
        raw.c_iflag |= IUTF8;
    #endif
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        return NULL;
    }
    
    char buf[MAXIMUM_INPUT] = {0};
    int len = 0;
    int pos = 0;
    int hist_off = 0;
    int history_total = history_count();
    char saved[MAXIMUM_INPUT] = {0};
    
    /* Panel state */
    int panel_sel = -1;
    int panel_rows = 0;
    char **panel_items = NULL;
    int panel_count = 0;

    panel_render(panel_items, panel_count, panel_sel, &panel_rows);
    render_with_suggestion(prompt, buf, len, pos);

    
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
            return NULL;
        }
        
        /* TAB */
        if (c == '\t') {
            char *sug = find_suggestion(buf, len);
            if (sug) {
                int sug_len = strlen(sug);
                if (sug_len > len && sug_len < MAXIMUM_INPUT) {
                    strncpy(buf, sug, MAXIMUM_INPUT - 1);
                    len = sug_len; pos = len;
                    free(sug);
                    panel_free(panel_items, panel_count);
                    panel_rebuild(buf, len, pos, &panel_items, &panel_count);
                    panel_sel = -1;
                    panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                    render_with_suggestion(prompt, buf, len, pos);
                    continue;
                }
                free(sug);
            }

            if (panel_count == 0) {
                write(STDOUT_FILENO, "\a", 1);
                continue;
            }

            if (panel_count == 1 && panel_sel == -1) {
                int ws = pos;
                while (ws > 0 && buf[ws-1] != ' ') ws--;
                const char *match = panel_items[0];
                int match_len = strlen(match);
                int tail = len - pos;
                memmove(buf + ws + match_len, buf + pos, tail);
                memcpy(buf + ws, match, match_len);
                len = ws + match_len + tail;
                pos = ws + match_len;
                buf[len] = '\0';
                panel_free(panel_items, panel_count);
                panel_rebuild(buf, len, pos, &panel_items, &panel_count);
                panel_sel = -1;
                panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                render_with_suggestion(prompt, buf, len, pos);
                continue;
            }

            if (panel_sel == -1) {
                panel_sel = 0;
            } else {
                panel_sel = (panel_sel + 1) % panel_count;
            }
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            render_with_suggestion(prompt, buf, len, pos);
            continue;
        }
        
        /* Ctrl+D */
        if (c == 4) {
            if (len == 0) {
                panel_free(panel_items, panel_count);
                panel_render(NULL, 0, -1, &panel_rows);  /* clear panel */
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
                return NULL;
            } else {
                continue;
            }
        }
        
        /* Ctrl+C */
        if (c == 3) {
            memset(buf, 0, MAXIMUM_INPUT);
            len = 0;
            pos = 0;
            hist_off = 0;
            memset(saved, 0, MAXIMUM_INPUT);
            panel_free(panel_items, panel_count);
            panel_items = NULL; panel_count = 0; panel_sel = -1;
            panel_render(NULL, 0, -1, &panel_rows);
            write(STDOUT_FILENO, "\n", 1);
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            render_with_suggestion(prompt, buf, len, pos);
            continue;
        }
        
        /* Enter */
        if (c == '\r' || c == '\n') {
            write(STDOUT_FILENO, "\r", 1);
            write(STDOUT_FILENO, prompt, strlen(prompt));
            char tmp_buf[MAXIMUM_INPUT + 1];
            memcpy(tmp_buf, buf, len);
            tmp_buf[len] = '\0';
            write(STDOUT_FILENO, tmp_buf, len);
            write(STDOUT_FILENO, "\033[K", 3);
            if (panel_sel >= 0 && panel_items) {
                /* insert selected item into buf replacing current word */
                int ws = pos;
                while (ws > 0 && buf[ws-1] != ' ') ws--;
                const char *match = panel_items[panel_sel];
                int match_len = strlen(match);
                int tail = len - pos;
                memmove(buf + ws + match_len, buf + pos, tail);
                memcpy(buf + ws, match, match_len);
                len = ws + match_len + tail;
                pos = ws + match_len;
                buf[len] = '\0';
                panel_free(panel_items, panel_count);
                panel_rebuild(buf, len, pos, &panel_items, &panel_count);
                panel_sel = -1;
                panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                render_with_suggestion(prompt, buf, len, pos);
                continue;
            } else {
                buf[len] = '\0';
                panel_free(panel_items, panel_count);
                panel_render(NULL, 0, -1, &panel_rows);  /* clear panel */
                history_total++;   /* we just added one */
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
                write(STDOUT_FILENO, "\r\n", 2);
                if (len > 0) {
                    history_add(buf);
                }
                return strdup(buf);
            }
        }
        
        /* Backspace */
        if (c == 127 || c == 8) {
            if (pos > 0) {
                int back = utf8_prev_char_len(buf, pos);
                memmove(buf + pos - back, buf + pos, len - pos);
                len -= back;
                pos -= back;
                if (len == 0) {
                    hist_off = 0;
                    memset(saved, 0, MAXIMUM_INPUT);
                }
            }
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            render_with_suggestion(prompt, buf, len, pos);

            continue;
        }
        
        /* Ctrl+A */
        if (c == 1) {
            pos = 0;
            if (panel_sel >= 0) {
                panel_sel = -1;
                panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                render_with_suggestion(prompt, buf, len, pos);

                continue;
            }
            render_with_suggestion(prompt, buf, len, pos);
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            render_with_suggestion(prompt, buf, len, pos);

            continue;
        }
        
        /* Ctrl+E */
        if (c == 5) {
            pos = len;
            if (panel_sel >= 0) {
                panel_sel = -1;
                panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                render_with_suggestion(prompt, buf, len, pos);

                continue;
            }
            render_with_suggestion(prompt, buf, len, pos);
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            render_with_suggestion(prompt, buf, len, pos);

            continue;
        }
        
        /* Ctrl+F */
        if (c == 6) {
            if (pos < len) {
                pos += utf8_char_len(buf, pos);
                if (pos > len) pos = len;
            }
            if (panel_sel >= 0) {
                panel_sel = -1;
                panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                render_with_suggestion(prompt, buf, len, pos);

                continue;
            }
            render_with_suggestion(prompt, buf, len, pos);
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            render_with_suggestion(prompt, buf, len, pos);

            continue;
        }
        
        /* Ctrl+B */
        if (c == 2) {
            if (pos > 0) {
                pos -= utf8_prev_char_len(buf, pos);
                if (pos < 0) pos = 0;
            }
            if (panel_sel >= 0) {
                panel_sel = -1;
                panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                render_with_suggestion(prompt, buf, len, pos);

                continue;
            }
            render_with_suggestion(prompt, buf, len, pos);
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            render_with_suggestion(prompt, buf, len, pos);
            continue;
        }
        
        /* Ctrl+R — reverse history search */
        if (c == 18) {
            int search_rows = 0;
            char *result = search_history_interactive(prompt);
            for (int i = 0; i < 10; i++) {
                write(STDOUT_FILENO, "\033[B\033[K", 6);
            }
            /* geri çık */
            write(STDOUT_FILENO, "\033[10A", 5);
            write(STDOUT_FILENO, "\r\033[K", 4);

            if (result) {
                strncpy(buf, result, MAXIMUM_INPUT - 1);
                buf[MAXIMUM_INPUT - 1] = '\0';
                len = strlen(buf);
                pos = len;
                free(result);
            } else {
                memset(buf, 0, MAXIMUM_INPUT);
                len = 0;
                pos = 0;
            }
            hist_off = 0;
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            render_with_suggestion(prompt, buf, len, pos);
            continue;
        }

        /* ESC sequence */
        if (c == 27) {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;
            
            if (seq[0] == '[') {
                /* Yukarı ok */
                if (seq[1] == 'A') {
                    if (panel_sel >= 0) {
                        panel_sel--;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        render_with_suggestion(prompt, buf, len, pos);
                        continue;
                    } else {
                        if (hist_off == 0) {
                            strncpy(saved, buf, MAXIMUM_INPUT - 1);
                            saved[MAXIMUM_INPUT - 1] = '\0';
                        }
                        hist_off++;
                        char *h = history_get(hist_off);
                        if (h) {
                            strncpy(buf, h, MAXIMUM_INPUT - 1);
                            buf[MAXIMUM_INPUT - 1] = '\0';
                            len = strlen(buf);
                            pos = len;
                            free(h);
                        } else {
                            hist_off--;
                        }
                        write(STDOUT_FILENO, "\r\033[K", 4);
                        panel_show_history(hist_off, &panel_rows);
                        render_with_suggestion(prompt, buf, len, pos);

                        continue;
                    }
                }

                if (seq[1] == 'B') {
                    if (panel_sel == -1 && panel_count > 0 && hist_off == 0) {
                        panel_sel = 0;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        render_with_suggestion(prompt, buf, len, pos);

                        continue;
                    } else if (panel_sel >= 0) {
                        panel_sel = (panel_sel + 1) % panel_count;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        render_with_suggestion(prompt, buf, len, pos);

                        continue;
                    } else {
                        if (hist_off == 0) {
                            continue;
                        }
                        hist_off--;
                        if (hist_off == 0) {
                            strncpy(buf, saved, MAXIMUM_INPUT - 1);
                            buf[MAXIMUM_INPUT - 1] = '\0';
                            len = strlen(buf);
                            pos = len;
                        } else {
                            char *h = history_get(hist_off);
                            if (h) {
                                strncpy(buf, h, MAXIMUM_INPUT - 1);
                                buf[MAXIMUM_INPUT - 1] = '\0';
                                len = strlen(buf);
                                pos = len;
                                free(h);
                            }
                        }
                        render_with_suggestion(prompt, buf, len, pos);
                        if (hist_off > 0)
                            panel_show_history(hist_off, &panel_rows);
                        else {
                            /* clear history panel, show completion panel */
                            if (panel_rows > 0) {  /* already cleared by panel_show_history */
                                /* panel_rows already 0 after clear */
                            }
                            panel_free(panel_items, panel_count);
                            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
                            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        }
                        continue;
                    }
                }

                if (seq[1] == 'C') {
                    if (panel_sel >= 0) {
                        int term_width = 80;
                        struct winsize ws;
                        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
                            term_width = ws.ws_col;
                        int col_width = 0;
                        for (int i = 0; i < panel_count; i++) {
                            int l = utf8_display_len(panel_items[i]);
                            if (l > col_width) col_width = l;
                        }
                        col_width += 2;
                        int cols = term_width / col_width;
                        if (cols < 1) cols = 1;
                        panel_sel = (panel_sel + 1) % panel_count;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        render_with_suggestion(prompt, buf, len, pos);

                        continue;
                    } else {
                        if (pos < len) {
                            pos += utf8_char_len(buf, pos);
                            if (pos > len) pos = len;
                        }
                        render_with_suggestion(prompt, buf, len, pos);
                        panel_free(panel_items, panel_count);
                        panel_rebuild(buf, len, pos, &panel_items, &panel_count);
                        panel_sel = -1;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        render_with_suggestion(prompt, buf, len, pos);

                        continue;
                    }
                }
                
                /* Sol ok */
                if (seq[1] == 'D') {
                    if (panel_sel >= 0) {
                        int term_width = 80;
                        struct winsize ws;
                        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
                            term_width = ws.ws_col;
                        int col_width = 0;
                        for (int i = 0; i < panel_count; i++) {
                            int l = utf8_display_len(panel_items[i]);
                            if (l > col_width) col_width = l;
                        }
                        col_width += 2;
                        int cols = term_width / col_width;
                        if (cols < 1) cols = 1;
                        panel_sel = (panel_sel - 1 + panel_count) % panel_count;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        render_with_suggestion(prompt, buf, len, pos);

                        continue;
                    } else {
                        if (pos > 0) {
                            pos -= utf8_prev_char_len(buf, pos);
                            if (pos < 0) pos = 0;
                        }
                        render_with_suggestion(prompt, buf, len, pos);
                        panel_free(panel_items, panel_count);
                        panel_rebuild(buf, len, pos, &panel_items, &panel_count);
                        panel_sel = -1;
                        panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                        render_with_suggestion(prompt, buf, len, pos);

                        continue;
                    }
                }
            } else {
                /* plain ESC */
                if (panel_sel >= 0) {
                    panel_sel = -1;
                    panel_render(panel_items, panel_count, panel_sel, &panel_rows);
                    render_with_suggestion(prompt, buf, len, pos);

                    continue;
                }
            }
            continue;
        }
        
        /* ASCII printable */
        if ((unsigned char)c >= 32 && c < 127) {
            if (panel_sel >= 0) {
                panel_sel = -1;
            }
            if (len < MAXIMUM_INPUT - 1) {
                memmove(buf + pos + 1, buf + pos, len - pos);
                buf[pos] = c;
                len++; pos++;
            }
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            render_with_suggestion(prompt, buf, len, pos);

            continue;
        }

        /* UTF-8 multi-byte leading byte */
        if ((unsigned char)c >= 0xC0 && (unsigned char)c <= 0xF7) {
            /* determine sequence length */
            int nb;
            unsigned char uc = (unsigned char)c;
            if      (uc >= 0xF0) nb = 4;
            else if (uc >= 0xE0) nb = 3;
            else                  nb = 2;

            if (len + nb < MAXIMUM_INPUT - 1) {
                /* read remaining bytes */
                char seq[4];
                seq[0] = c;
                for (int b = 1; b < nb; b++) {
                    if (read(STDIN_FILENO, &seq[b], 1) <= 0) goto done;
                    /* must be continuation byte 10xxxxxx */
                    if (((unsigned char)seq[b] & 0xC0) != 0x80) goto done;
                }
                /* insert all nb bytes at cursor position */
                memmove(buf + pos + nb, buf + pos, len - pos);
                memcpy(buf + pos, seq, nb);
                len += nb;
                pos += nb;
            }
            panel_free(panel_items, panel_count);
            panel_rebuild(buf, len, pos, &panel_items, &panel_count);
            panel_sel = -1;
            panel_render(panel_items, panel_count, panel_sel, &panel_rows);
            render_with_suggestion(prompt, buf, len, pos);

            continue;
        }
    }
    
    done: ;   /* UTF-8 error recovery */
}

char *read_heredoc(const char *delimiter, int expand) {
    /*
     * Reads lines from stdin until a line matching delimiter exactly.
     * Returns malloc'd string with content, caller must free().
     * expand parameter reserved for future use (expansion handled in expand.c).
     */

    size_t buf_cap = 4096;
    char *buf = malloc(buf_cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t buf_len = 0;

    char line[MAXIMUM_INPUT];
    int delim_len = strlen(delimiter);

    /* save and restore terminal — we need cooked mode for here-doc input */

    /* heredoc reads Ctrl+C as EOF — no special handling needed
       since we're in cooked mode, Ctrl+C sends SIGINT which
       interrupts fgets and returns NULL */
    /* show continuation prompt */
    write(STDOUT_FILENO, "> ", 2);
    while (1) {

        if (!fgets(line, sizeof(line), stdin)) {
            /* EOF or error — treat as if delimiter was found */
            break;
        }

        /* strip trailing newline for comparison */
        int line_len = strlen(line);
        if (line_len > 0 && line[line_len-1] == '\n') {
            line[line_len-1] = '\0';
            line_len--;
        }

        /* check for delimiter */
        if (line_len == delim_len &&
            strcmp(line, delimiter) == 0) {
            break;
        }

        /* after the delimiter check, before appending: */
        write(STDOUT_FILENO, "> ", 2);

        /* append line + newline to buf */
        size_t needed = buf_len + line_len + 2; /* +2 for \n and \0 */
        if (needed > buf_cap) {
            while (buf_cap < needed) buf_cap *= 2;
            char *tmp = realloc(buf, buf_cap);
            if (!tmp) { free(buf); buf = NULL; break; }
            buf = tmp;
        }
        memcpy(buf + buf_len, line, line_len);
        buf_len += line_len;
        buf[buf_len++] = '\n';
        buf[buf_len] = '\0';
    }
    

    return buf;  /* may be NULL on alloc failure */
}
