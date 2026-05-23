//
// highlight.c — syntax error highlighting eklendi
// Orijinal renklendirme sistemi korundu, üstüne hata tespiti eklendi.
//

#include "../include/highlight.h"
#include "../include/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  Color name → ANSI escape                                            */
/* ------------------------------------------------------------------ */

static const char *color_to_ansi(const char *name) {
    if (!name || !*name)            return "\033[0m";
    if (strcmp(name,"reset")   ==0) return "\033[0m";
    if (strcmp(name,"bold")    ==0) return "\033[1m";
    if (strcmp(name,"dim")     ==0) return "\033[2;37m";
    if (strcmp(name,"red")     ==0) return "\033[31m";
    if (strcmp(name,"bold_red")==0) return "\033[1;31m";
    if (strcmp(name,"green")   ==0) return "\033[32m";
    if (strcmp(name,"yellow")  ==0) return "\033[33m";
    if (strcmp(name,"blue")    ==0) return "\033[34m";
    if (strcmp(name,"magenta") ==0) return "\033[35m";
    if (strcmp(name,"cyan")    ==0) return "\033[36m";
    if (strcmp(name,"white")   ==0) return "\033[37m";
    if (strcmp(name,"gray")    ==0) return "\033[2;37m";
    if (strcmp(name,"none")    ==0) return "";
    return "\033[0m";
}

#define COL_RESET       "\033[0m"
#define COL_ERR_STR     "\033[31m"          /* unclosed quote       */
#define COL_ERR_BRACE   "\033[31m"          /* unclosed ${ $(       */
#define COL_ERR_KW      "\033[1;31m"        /* unmatched fi/done/   */
#define COL_WARN_OPEN   "\033[33m"          /* unclosed block kw    */

/* ------------------------------------------------------------------ */
/*  Keyword table                                                       */
/* ------------------------------------------------------------------ */

static int is_keyword(const char *word) {
    static const char *kws[] = {
        "if","then","else","elif","fi",
        "while","until","do","done",
        "for","in",
        "case","esac",
        "function","return","break","continue",
        "local","export","unset",
        NULL
    };
    for (int i = 0; kws[i]; i++)
        if (strcmp(word, kws[i]) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Dangerous command table                                             */
/* ------------------------------------------------------------------ */

static int is_dangerous(const char *cmd) {
    static const char *dangerous[] = {
        "rm","rmdir","dd","mkfs","fdisk",
        "chmod","chown","kill","killall", NULL
    };
    for (int i = 0; dangerous[i]; i++)
        if (strcmp(cmd, dangerous[i]) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Command color lookup                                                */
/* ------------------------------------------------------------------ */

static const char *cmd_color(const char *word) {
    if (is_dangerous(word)) return "\033[1;31m";

    extern int is_builtin(const char *cmd);
    if (is_builtin(word)) return color_to_ansi(g_config.hl_color_cmd_ok);

    extern char *alias_expand(const char *name);
    if (alias_expand(word)) return color_to_ansi(g_config.hl_color_cmd_ok);

    if (word[0] == '/' || word[0] == '.') {
        return access(word, X_OK) == 0
               ? color_to_ansi(g_config.hl_color_cmd_ok)
               : color_to_ansi(g_config.hl_color_cmd_err);
    }

    char *path_env = getenv("PATH");
    if (path_env) {
        char path_copy[4096];
        strncpy(path_copy, path_env, sizeof(path_copy)-1);
        char *dir = strtok(path_copy, ":");
        while (dir) {
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", dir, word);
            if (access(full, X_OK) == 0)
                return color_to_ansi(g_config.hl_color_cmd_ok);
            dir = strtok(NULL, ":");
        }
    }
    return color_to_ansi(g_config.hl_color_cmd_err);
}

/* ------------------------------------------------------------------ */
/*  Output buffer                                                       */
/* ------------------------------------------------------------------ */

typedef struct { char *buf; size_t len; size_t cap; } Obuf;

static void ob_init(Obuf *o, size_t cap) {
    o->buf = malloc(cap); o->len = 0; o->cap = cap;
    if (o->buf) o->buf[0] = '\0';
}
static void ob_append(Obuf *o, const char *s, size_t slen) {
    if (!o->buf || !s || slen == 0) return;
    while (o->len + slen + 1 > o->cap) {
        o->cap *= 2;
        char *tmp = realloc(o->buf, o->cap);
        if (!tmp) return;
        o->buf = tmp;
    }
    memcpy(o->buf + o->len, s, slen);
    o->len += slen;
    o->buf[o->len] = '\0';
}
static void ob_str(Obuf *o, const char *s) { if (s) ob_append(o, s, strlen(s)); }
static void ob_ch (Obuf *o, char c)        { ob_append(o, &c, 1); }

/* ------------------------------------------------------------------ */
/*  Pre-scan: detect errors in the buffer BEFORE rendering             */
/*                                                                      */
/*  Returns a SyntaxErrors struct describing what went wrong.           */
/*  highlight() uses this to decide where to apply error colors.        */
/* ------------------------------------------------------------------ */

typedef struct {
    int unclosed_single;    /* 1 = odd number of unescaped '       */
    int unclosed_double;    /* 1 = odd number of unescaped "       */
    int unclosed_brace;     /* depth of ${ — positive = unclosed  */
    int unclosed_paren;     /* depth of $( or ( — positive = open */
    int block_depth;        /* if/while/for/case depth             */
    int extra_close;        /* fi/done/esac without opener         */
    /*
     * Positions of the first unclosed opener in the original buffer.
     * -1 = not set.
     */
    int first_unclosed_sq;  /* pos of unclosed '  */
    int first_unclosed_dq;  /* pos of unclosed "  */
    int first_unclosed_br;  /* pos of unclosed ${ */
    int first_unclosed_pa;  /* pos of unclosed $( */
} SyntaxErrors;

static SyntaxErrors prescan(const char *buf, int len) {
    SyntaxErrors e = {0,0,0,0,0,0,-1,-1,-1,-1};

    int in_sq = 0, in_dq = 0;
    /* block depth stack — track opening keyword positions */
    int block_stack[64];
    int block_top = 0;

    /* brace/paren depth stacks */
    int brace_stack[64], brace_top = 0;
    int paren_stack[64], paren_top = 0;

    const char *p = buf;
    const char *end = buf + len;

    while (p < end) {
        /* inside single quote: only ' closes it */
        if (in_sq) {
            if (*p == '\'') { in_sq = 0; }
            p++;
            continue;
        }

        /* inside double quote: " closes, $ handled, \ escapes */
        if (in_dq) {
            if (*p == '"')  { in_dq = 0; p++; continue; }
            if (*p == '\\' && p+1 < end) { p += 2; continue; }
            if (*p == '$') {
                p++;
                if (p < end && *p == '{') {
                    if (brace_top < 63) brace_stack[brace_top++] = (int)(p - buf);
                    p++;
                } else if (p < end && *p == '(') {
                    if (paren_top < 63) paren_stack[paren_top++] = (int)(p - buf);
                    p++;
                }
                continue;
            }
            if (*p == '}' && brace_top > 0) brace_top--;
            if (*p == ')' && paren_top > 0) paren_top--;
            p++;
            continue;
        }

        /* comment: skip to end of line */
        if (*p == '#') {
            while (p < end && *p != '\n') p++;
            continue;
        }

        /* escape outside quotes */
        if (*p == '\\' && p+1 < end) { p += 2; continue; }

        /* single quote open */
        if (*p == '\'') {
            in_sq = 1;
            if (e.first_unclosed_sq == -1) e.first_unclosed_sq = (int)(p - buf);
            p++;
            continue;
        }

        /* double quote open */
        if (*p == '"') {
            in_dq = 1;
            if (e.first_unclosed_dq == -1) e.first_unclosed_dq = (int)(p - buf);
            p++;
            continue;
        }

        /* ${ open */
        if (*p == '$' && p+1 < end && *(p+1) == '{') {
            if (e.first_unclosed_br == -1) e.first_unclosed_br = (int)(p - buf);
            if (brace_top < 63) brace_stack[brace_top++] = (int)(p - buf);
            p += 2;
            continue;
        }

        /* } close brace */
        if (*p == '}') {
            if (brace_top > 0) {
                brace_top--;
                /* if we just closed the first unclosed, clear the marker */
                if (brace_top == 0) e.first_unclosed_br = -1;
            }
            p++;
            continue;
        }

        /* $( or ( open paren */
        if ((*p == '$' && p+1 < end && *(p+1) == '(') || *p == '(') {
            if (e.first_unclosed_pa == -1) e.first_unclosed_pa = (int)(p - buf);
            if (paren_top < 63) paren_stack[paren_top++] = (int)(p - buf);
            if (*p == '$') p++;
            p++;
            continue;
        }

        /* ) close paren */
        if (*p == ')') {
            if (paren_top > 0) {
                paren_top--;
                if (paren_top == 0) e.first_unclosed_pa = -1;
            }
            p++;
            continue;
        }

        /* word — check for block keywords */
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *ws = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int wlen = (int)(p - ws);

            /* check for keyword */
            char word[64] = {0};
            if (wlen < 63) { memcpy(word, ws, wlen); word[wlen] = '\0'; }

            /* openers */
            if (strcmp(word,"if")==0    || strcmp(word,"while")==0 ||
                strcmp(word,"until")==0 || strcmp(word,"for")==0   ||
                strcmp(word,"case")==0  ||
                /* function body { counts too */
                strcmp(word,"function")==0) {
                if (block_top < 63)
                    block_stack[block_top++] = (int)(ws - buf);
            }
            /* closers */
            else if (strcmp(word,"fi")==0   || strcmp(word,"done")==0 ||
                     strcmp(word,"esac")==0) {
                if (block_top > 0) block_top--;
                else e.extra_close++;
            }

            continue;
        }

        p++;
    }

    e.unclosed_single = in_sq;
    e.unclosed_double = in_dq;
    e.unclosed_brace  = brace_top;
    e.unclosed_paren  = paren_top;
    e.block_depth     = block_top;

    /* if quote closed normally, clear the position markers */
    if (!e.unclosed_single) e.first_unclosed_sq = -1;
    if (!e.unclosed_double) e.first_unclosed_dq = -1;
    if (!e.unclosed_brace)  e.first_unclosed_br = -1;
    if (!e.unclosed_paren)  e.first_unclosed_pa = -1;

    (void)block_stack; (void)brace_stack; (void)paren_stack;
    return e;
}

/* ------------------------------------------------------------------ */
/*  Main highlight function                                             */
/* ------------------------------------------------------------------ */

char *highlight(const char *buf, int len) {
    if (!g_config.highlight_enabled) {
        char *r = malloc(len + 1);
        if (r) { memcpy(r, buf, len); r[len] = '\0'; }
        return r;
    }
    if (!buf || len == 0) return strdup("");

    /* pre-scan for errors */
    SyntaxErrors err = prescan(buf, len);

    Obuf o;
    ob_init(&o, len * 8 + 256);
    if (!o.buf) return strdup(buf);

    const char *p   = buf;
    const char *end = buf + len;
    int is_first_word = 1;
    int after_pipe    = 0;

    while (p < end) {
        int cur_pos = (int)(p - buf);

        /* ── comment ──────────────────────────────────────────────── */
        if (*p == '#') {
            ob_str(&o, color_to_ansi(g_config.hl_color_comment));
            while (p < end && *p != '\n') ob_ch(&o, *p++);
            ob_str(&o, COL_RESET);
            continue;
        }

        /* ── whitespace ───────────────────────────────────────────── */
        if (*p == ' ' || *p == '\t' || *p == '\n') {
            ob_ch(&o, *p++);
            continue;
        }

        /* ── operators ────────────────────────────────────────────── */
        if (*p == '|' || *p == '>' || *p == '<' ||
            *p == '&' || *p == ';') {
            ob_str(&o, color_to_ansi(g_config.hl_color_operator));
            ob_ch(&o, *p);
            if ((*p == '>' && *(p+1) == '>') ||
                (*p == '&' && *(p+1) == '&') ||
                (*p == '|' && *(p+1) == '|')) {
                p++; ob_ch(&o, *p);
            }
            ob_str(&o, COL_RESET);
            p++;
            if (*(p-1) == '|' || *(p-1) == ';') {
                is_first_word = 1; after_pipe = 1;
            }
            continue;
        }

        /* ── single-quoted string ─────────────────────────────────── */
        if (*p == '\'') {
            /*
             * Error case: this is the unclosed opening quote.
             * Highlight everything from here to end in error color.
             */
            int is_err = (err.unclosed_single &&
                          cur_pos == err.first_unclosed_sq);
            ob_str(&o, is_err ? COL_ERR_STR
                              : color_to_ansi(g_config.hl_color_string));
            ob_ch(&o, *p++);
            while (p < end && *p != '\'') ob_ch(&o, *p++);
            if (p < end) ob_ch(&o, *p++); /* closing ' */
            ob_str(&o, COL_RESET);
            is_first_word = 0;
            continue;
        }

        /* ── double-quoted string ─────────────────────────────────── */
        if (*p == '"') {
            int is_err = (err.unclosed_double &&
                          cur_pos == err.first_unclosed_dq);
            ob_str(&o, is_err ? COL_ERR_STR
                              : color_to_ansi(g_config.hl_color_string));
            ob_ch(&o, *p++);
            while (p < end && *p != '"') {
                if (*p == '$') {
                    ob_str(&o, color_to_ansi(g_config.hl_color_variable));
                    ob_ch(&o, *p++);
                    if (p < end && *p == '{') {
                        ob_ch(&o, *p++);
                        while (p < end && *p != '}') ob_ch(&o, *p++);
                        if (p < end) ob_ch(&o, *p++);
                    } else if (p < end && *p == '(') {
                        ob_ch(&o, *p++);
                        int d = 1;
                        while (p < end && d > 0) {
                            if (*p == '(') d++;
                            else if (*p == ')') d--;
                            ob_ch(&o, *p++);
                        }
                    } else {
                        while (p < end &&
                               (isalnum((unsigned char)*p) || *p == '_'))
                            ob_ch(&o, *p++);
                    }
                    /* restore string color */
                    ob_str(&o, is_err ? COL_ERR_STR
                                      : color_to_ansi(g_config.hl_color_string));
                } else if (*p == '\\' && *(p+1)) {
                    ob_ch(&o, *p++); ob_ch(&o, *p++);
                } else {
                    ob_ch(&o, *p++);
                }
            }
            if (p < end) ob_ch(&o, *p++);
            ob_str(&o, COL_RESET);
            is_first_word = 0;
            continue;
        }

        /* ── variable: $VAR ${VAR} $(...) $((...)) ────────────────── */
        if (*p == '$') {
            /*
             * Unclosed ${ or $( → error color for the opener.
             */
            int is_brace_err = (err.unclosed_brace > 0 &&
                                 cur_pos == err.first_unclosed_br);
            int is_paren_err = (err.unclosed_paren > 0 &&
                                 cur_pos == err.first_unclosed_pa);
            int is_err_var   = is_brace_err || is_paren_err;

            ob_str(&o, is_err_var ? COL_ERR_BRACE
                                  : color_to_ansi(g_config.hl_color_variable));
            ob_ch(&o, *p++);

            if (p < end && *p == '{') {
                ob_ch(&o, *p++);
                while (p < end && *p != '}') ob_ch(&o, *p++);
                if (p < end) ob_ch(&o, *p++);
            } else if (p < end && *p == '(' && *(p+1) == '(') {
                ob_ch(&o, *p++); ob_ch(&o, *p++);
                int d = 2;
                while (p < end && d > 0) {
                    if (*p == '(') d++;
                    else if (*p == ')') d--;
                    ob_ch(&o, *p++);
                }
            } else if (p < end && *p == '(') {
                ob_ch(&o, *p++);
                int d = 1;
                while (p < end && d > 0) {
                    if (*p == '(') d++;
                    else if (*p == ')') d--;
                    ob_ch(&o, *p++);
                }
            } else {
                while (p < end &&
                       (isalnum((unsigned char)*p) || *p == '_' ||
                        *p == '?' || *p == '#' || *p == '@' || *p == '*' ||
                        (*p >= '0' && *p <= '9')))
                    ob_ch(&o, *p++);
            }
            ob_str(&o, COL_RESET);
            is_first_word = 0;
            continue;
        }

        /* ── word ─────────────────────────────────────────────────── */
        {
            const char *ws = p;
            while (p < end &&
                   *p != ' ' && *p != '\t' && *p != '\n' &&
                   *p != '|' && *p != '>' && *p != '<' &&
                   *p != '&' && *p != ';' &&
                   *p != '\'' && *p != '"' && *p != '$' && *p != '#') {
                if (*p == '$' && *(p+1) == '(') break;
                p++;
            }
            int wlen = (int)(p - ws);
            if (wlen == 0) { p++; continue; }

            char word[1024] = {0};
            if (wlen < (int)sizeof(word))
                memcpy(word, ws, wlen);
            else
                memcpy(word, ws, sizeof(word)-1);

            const char *col = NULL;

            if (is_first_word || after_pipe) {
                if (strchr(word, '=') && word[0] != '=') {
                    col = NULL;
                    is_first_word = 0; after_pipe = 0;
                } else if (is_keyword(word)) {
                    /*
                     * Closing keyword with no matching opener → error.
                     * Opening keyword when block is already unclosed →
                     * warn color only if this is the LAST unclosed opener
                     * (i.e. the one still open at end of input).
                     */
                    int is_closer = (strcmp(word,"fi")==0   ||
                                     strcmp(word,"done")==0 ||
                                     strcmp(word,"esac")==0);
                    int is_opener = (strcmp(word,"if")==0    ||
                                     strcmp(word,"while")==0 ||
                                     strcmp(word,"until")==0 ||
                                     strcmp(word,"for")==0   ||
                                     strcmp(word,"case")==0);

                    if (is_closer && err.extra_close > 0) {
                        col = COL_ERR_KW;   /* unmatched closer */
                    } else if (is_opener && err.block_depth > 0) {
                        /*
                         * Block still open at end of buffer.
                         * Show opener in warning amber — input is
                         * being typed (multiline continues), not
                         * an error yet, just "still open".
                         */
                        col = COL_WARN_OPEN;
                    } else {
                        col = color_to_ansi(g_config.hl_color_keyword);
                    }

                    if (strcmp(word,"then")==0 || strcmp(word,"else")==0 ||
                        strcmp(word,"do")==0   || strcmp(word,"elif")==0) {
                        after_pipe = 1; is_first_word = 1;
                    } else {
                        after_pipe = 0; is_first_word = 0;
                    }
                } else {
                    col = cmd_color(word);
                    is_first_word = 0; after_pipe = 0;
                }
            } else {
                /* argument */
                if (word[0] == '-') {
                    col = color_to_ansi(g_config.hl_color_flag);
                } else if (word[0] == '/' || word[0] == '~' ||
                           word[0] == '.' || strchr(word, '/')) {
                    col = color_to_ansi(g_config.hl_color_path);
                } else {
                    col = NULL;
                }
            }

            if (col && *col) ob_str(&o, col);
            ob_append(&o, ws, wlen);
            if (col && *col) ob_str(&o, COL_RESET);
        }
    }

    return o.buf;
}