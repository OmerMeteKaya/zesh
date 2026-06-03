// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../include/shell.h"

void tokens_free(Token *toks, int n) {
    if (!toks) return;
    for (int i = 0; i < n; i++) {
        if (toks[i].value) {
            free(toks[i].value);
        }
    }
    free(toks);
}


static char *strdup_range(const char *start, const char *end) {
    size_t len = end - start;
    char *result = malloc(len + 1);
    if (result) {
        memcpy(result, start, len);
        result[len] = '\0';
    }
    return result;
}

static Token *add_token(Token **tokens, int *count, int *capacity, TokenType type, char *value) {
    if (*count >= *capacity) {
        *capacity *= 2;
        Token *tmp = realloc(*tokens, *capacity * sizeof(Token));
        if (!tmp) {
            free(*tokens);
            return NULL;
        }
        *tokens = tmp;
    }
    (*tokens)[*count].type = type;
    (*tokens)[*count].value = value;
    (*tokens)[*count].quoted = 0;
    (*count)++;
    return *tokens;
}

Token *lex(const char *input, int *ntokens) {
    Token *tokens = malloc(16 * sizeof(Token));
    if (!tokens) return NULL;
    
    int count = 0;
    int capacity = 16;
    const char *p = input;

    while (*p) {
        if (*p == '\n') {
            if (!add_token(&tokens, &count, &capacity, TOK_SEMI, NULL)) return NULL;
            p++;
            continue;
        }
        // Skip whitespace
        if (isspace(*p)) {
            p++;
            continue;
        }

        // Single character operators
        switch (*p) {
            case '&':
                if (*(p+1) == '&') {
                    if (!add_token(&tokens, &count, &capacity, TOK_AND, NULL)) return NULL;
                    p += 2;
                } else {
                    if (!add_token(&tokens, &count, &capacity, TOK_BG, NULL)) return NULL;
                    p++;
                }
                continue;
            case '|':
                if (*(p+1) == '|') {
                    if (!add_token(&tokens, &count, &capacity, TOK_OR, NULL)) return NULL;
                    p += 2;
                } else {
                    if (!add_token(&tokens, &count, &capacity, TOK_PIPE, NULL)) return NULL;
                    p++;
                }
                continue;
            case ';':
                if (!add_token(&tokens, &count, &capacity, TOK_SEMI, NULL)) return NULL;
                p++;
                continue;
            case '[':
                if (*(p+1) == '[') {
                    if (!add_token(&tokens, &count, &capacity,
                                   TOK_DOUBLE_LBRACKET, NULL)) return NULL;
                    p += 2;
                } else {
                    char *v = strdup("[");
                    if (!v) return NULL;
                    if (!add_token(&tokens, &count, &capacity, TOK_WORD, v))
                        return NULL;
                    p++;
                }
                continue;

            case ']':
                if (*(p+1) == ']') {
                    if (!add_token(&tokens, &count, &capacity,
                                   TOK_DOUBLE_RBRACKET, NULL)) return NULL;
                    p += 2;
                } else {
                    char *v = strdup("]");
                    if (!v) return NULL;
                    if (!add_token(&tokens, &count, &capacity, TOK_WORD, v))
                        return NULL;
                    p++;
                }
                continue;
            case '{':
                /* { tek başına keyword olarak word token */
            {
                char *v = strdup("{");
                if (!v) return NULL;
                if (!add_token(&tokens, &count, &capacity, TOK_WORD, v))
                    return NULL;
                p++;
            }
                continue;

            case '}':
            {
                char *v = strdup("}");
                if (!v) return NULL;
                if (!add_token(&tokens, &count, &capacity, TOK_WORD, v))
                    return NULL;
                p++;
            }
                continue;
            case '(':
                if (*(p+1) == '(') {
                    if (!add_token(&tokens, &count, &capacity,
                                   TOK_DOUBLE_LPAREN, NULL)) return NULL;
                    p += 2;
                } else {
                    char *v = strdup("(");
                    if (!v) return NULL;
                    if (!add_token(&tokens, &count, &capacity, TOK_WORD, v))
                        return NULL;
                    p++;
                }
                continue;

            case ')':
                if (*(p+1) == ')') {
                    if (!add_token(&tokens, &count, &capacity,
                                   TOK_DOUBLE_RPAREN, NULL)) return NULL;
                    p += 2;
                } else {
                    char *v = strdup(")");
                    if (!v) return NULL;
                    if (!add_token(&tokens, &count, &capacity, TOK_WORD, v))
                        return NULL;
                    p++;
                }
                continue;
            case '<':
                if (*(p+1) == '&') {
                    /* <&N or <&- or <&$var : use TOK_REDIR_DUP_IN with word following */
                    p += 2;
                    if (!add_token(&tokens, &count, &capacity, TOK_REDIR_DUP_IN, strdup("0")))
                        return NULL;
                    continue;
                } else if (*(p+1) == '<') {
                    /* here-string: <<< word */
                    if (*(p+2) == '<') {
                        p += 3;
                        while (*p == ' ' || *p == '\t') p++;
                        const char *hs_start = p;
                        /* read until whitespace or end, respecting quotes */
                        char hsbuf[MAX_INPUT] = {0};
                        int  hsi = 0;
                        if (*p == '\'' || *p == '"') {
                            char q = *p++;
                            while (*p && *p != q)
                                hsbuf[hsi++] = *p++;
                            if (*p == q) p++;
                        } else {
                            while (*p && !isspace(*p) &&
                                   *p != '|' && *p != '&' && *p != ';')
                                hsbuf[hsi++] = *p++;
                        }
                        hsbuf[hsi] = '\0';
                        (void)hs_start;
                        char *val = strdup(hsbuf);
                        if (!val) return NULL;
                        if (!add_token(&tokens, &count, &capacity,
                                       TOK_HERESTRING, val)) return NULL;
                        continue;
                    }
                    /* here-doc: << DELIM or <<'DELIM' */
                    p += 2;
                    /* skip whitespace */
                    while (*p == ' ' || *p == '\t') p++;

                    int noexp = 0;
                    /* check for quoted delimiter */
                    if (*p == '\'' || *p == '"') {
                        noexp = 1;
                        p++; /* skip opening quote */
                    }

                    /* read delimiter word */
                    const char *delim_start = p;
                    while (*p && !isspace(*p) && *p != '\'' && *p != '"') p++;

                    char *delim = strndup(delim_start, p - delim_start);
                    if (!delim) return NULL;

                    /* skip closing quote if present */
                    if (*p == '\'' || *p == '"') p++;

                    TokenType htype = noexp ? TOK_HEREDOC_NOEXP : TOK_HEREDOC;
                    if (!add_token(&tokens, &count, &capacity, htype, delim))
                        return NULL;
                    continue;
                } else if (*(p+1) == '(') {
                    const char *ps_start = p;
                    p += 2;  /* skip <( */
                    int depth = 1;
                    while (*p && depth > 0) {
                        if (*p == '(') depth++;
                        else if (*p == ')') depth--;
                        p++;
                    }
                    char *value = strdup_range(ps_start, p);
                    if (!value) { tokens_free(tokens, count); return NULL; }
                    if (!add_token(&tokens, &count, &capacity, TOK_WORD, value))
                        return NULL;
                    continue;
                } else {
                    if (!add_token(&tokens, &count, &capacity, TOK_REDIR_IN, NULL))
                        return NULL;
                    p++;
                    continue;
                }
            case '>':
                if (*(p+1) == '>') {
                    if (!add_token(&tokens, &count, &capacity, TOK_REDIR_APP, NULL)) return NULL;
                    p += 2;
                } else if (*(p+1) == '&') {
                    /* >&N or >&- or >&$var : use TOK_REDIR_DUP_OUT with word following */
                    p += 2;
                    /* emit TOK_REDIR_DUP_OUT with src "1"; the target word follows */
                    if (!add_token(&tokens, &count, &capacity, TOK_REDIR_DUP_OUT, strdup("1")))
                        return NULL;
                    continue;
                } else if (*(p+1) == '(') {
                    /* process substitution >(...) */
                    const char *ps_start = p;
                    p += 2;
                    int depth = 1;
                    while (*p && depth > 0) {
                        if (*p == '(') depth++;
                        else if (*p == ')') depth--;
                        p++;
                    }
                    char *value = strdup_range(ps_start, p);
                    if (!value) { tokens_free(tokens, count); return NULL; }
                    if (!add_token(&tokens, &count, &capacity, TOK_WORD, value))
                        return NULL;
                    continue;
                } else {
                    if (!add_token(&tokens, &count, &capacity, TOK_REDIR_OUT, NULL))
                        return NULL;
                    p++;
                    continue;
                }
        }

        /* $'...' ANSI-C quoting */
        if (*p == '$' && *(p+1) == '\'') {
            p += 2;  /* skip $' */
            char *buf2 = malloc(strlen(input) + 1);
            if (!buf2) { tokens_free(tokens, count); return NULL; }
            size_t bi = 0;
            while (*p && *p != '\'') {
                if (*p == '\\' && *(p+1)) {
                    p++;
                    switch (*p) {
                        case 'n':  buf2[bi++] = '\n'; break;
                        case 't':  buf2[bi++] = '\t'; break;
                        case 'r':  buf2[bi++] = '\r'; break;
                        case 'a':  buf2[bi++] = '\a'; break;
                        case 'b':  buf2[bi++] = '\b'; break;
                        case 'f':  buf2[bi++] = '\f'; break;
                        case 'v':  buf2[bi++] = '\v'; break;
                        case 'e':
                        case 'E':  buf2[bi++] = '\033'; break;
                        case '\\': buf2[bi++] = '\\'; break;
                        case '\'': buf2[bi++] = '\''; break;
                        case '"':  buf2[bi++] = '"'; break;
                        case '0': case '1': case '2': case '3':
                        case '4': case '5': case '6': case '7': {
                            unsigned val = *p - '0';
                            if (*(p+1) >= '0' && *(p+1) <= '7') { p++; val = val*8 + (*p-'0'); }
                            if (*(p+1) >= '0' && *(p+1) <= '7') { p++; val = val*8 + (*p-'0'); }
                            buf2[bi++] = (char)val;
                            break;
                        }
                        case 'x': {
                            p++;
                            unsigned val = 0;
                            int n = 0;
                            while (n < 2 && isxdigit((unsigned char)*p)) {
                                val = val*16 + (isdigit((unsigned char)*p) ?
                                    (*p-'0') : (tolower((unsigned char)*p)-'a'+10));
                                p++; n++;
                            }
                            p--;
                            buf2[bi++] = (char)val;
                            break;
                        }
                        default: buf2[bi++] = '\\'; buf2[bi++] = *p; break;
                    }
                    p++;
                } else {
                    buf2[bi++] = *p++;
                }
            }
            buf2[bi] = '\0';
            if (*p == '\'') p++;
            char *value = strdup(buf2);
            free(buf2);
            if (!value) { tokens_free(tokens, count); return NULL; }
            if (!add_token(&tokens, &count, &capacity, TOK_WORD, value)) return NULL;
            tokens[count-1].quoted = 1;
            continue;
        }

        // Quoted strings
        if (*p == '\'') {
            const char *start = ++p;
            while (*p && *p != '\'') p++;
            if (*p == '\'') {
                size_t len = p - start;
                char *value = malloc(len + 3);
                if (!value) {
                    tokens_free(tokens, count);
                    return NULL;
                }
                value[0] = '\'';
                memcpy(value + 1, start, len);
                value[len + 1] = '\'';
                value[len + 2] = '\0';
                if (!add_token(&tokens, &count, &capacity, TOK_WORD, value)) return NULL;
                tokens[count-1].quoted = 1;
                p++;
            } else {
                tokens_free(tokens, count);
                return NULL; // Unterminated string
            }
            continue;
        }

        if (*p == '"') {
            ++p;
            char *buffer = malloc(strlen(input) + 1);
            if (!buffer) {
                tokens_free(tokens, count);
                return NULL;
            }
            char *buf_p = buffer;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p+1) == '"') {
                    *buf_p++ = '"';
                    p += 2;
                } else {
                    *buf_p++ = *p++;
                }
            }
            *buf_p = '\0';
            if (*p == '"') {
                char *value = strdup(buffer);
                free(buffer);
                if (!value) {
                    tokens_free(tokens, count);
                    return NULL;
                }
                if (!add_token(&tokens, &count, &capacity, TOK_WORD, value)) return NULL;
                tokens[count-1].quoted = 1;
                p++;
            } else {
                free(buffer);
                tokens_free(tokens, count);
                return NULL; // Unterminated string
            }
            continue;
        }
        /* fd redirection: N>file, N>>file, N<file, N>&M, N<&M, N<&-, N>&- */
        /* Also {name}>file */
        if (isdigit((unsigned char)*p) || *p == '{') {
            const char *fd_start = p;
            char fd_name[64] = {0};
            int  fd_num = -1;
            int  is_named = 0;

            if (*p == '{') {
                p++;
                int ni = 0;
                while (*p && *p != '}' && ni < 63) fd_name[ni++] = *p++;
                fd_name[ni] = '\0';
                if (*p == '}') { p++; is_named = 1; }
                else { p = fd_start; goto word_parse; }
            } else {
                /* read digits */
                int ni = 0;
                char nbuf[16] = {0};
                while (isdigit((unsigned char)*p) && ni < 15) nbuf[ni++] = *p++;
                if (*p == '>' || *p == '<') {
                    fd_num = atoi(nbuf);
                } else {
                    p = fd_start;
                    goto word_parse;
                }
            }

            if (*p == '>') {
                p++;
                int is_app = 0;
                if (*p == '>') { is_app = 1; p++; }
                if (*p == '&') {
                    /* N>&M or N>&- */
                    p++;
                    char tgt[16] = {0};
                    int ti = 0;
                    if (*p == '-') { tgt[0] = '-'; ti = 1; p++; }
                    else while (isdigit((unsigned char)*p) && ti < 15) tgt[ti++] = *p++;
                    tgt[ti] = '\0';
                    /* encode as WORD "Nfd>&Mfd" and use TOK_REDIR_DUP_OUT */
                    char enc[64];
                    if (is_named)
                        snprintf(enc, sizeof(enc), "{%s}&%s", fd_name, tgt);
                    else
                        snprintf(enc, sizeof(enc), "%d&%s", fd_num, tgt);
                    if (!add_token(&tokens, &count, &capacity, TOK_REDIR_DUP_OUT, strdup(enc)))
                        return NULL;
                    continue;
                }
                /* N>file or N>>file: encode fd in token value, file follows */
                char enc[64];
                if (is_named)
                    snprintf(enc, sizeof(enc), "{%s}", fd_name);
                else
                    snprintf(enc, sizeof(enc), "%d", fd_num);
                TokenType tt2 = is_app ? TOK_REDIR_FD_APP : TOK_REDIR_FD_OUT;
                if (!add_token(&tokens, &count, &capacity, tt2, strdup(enc)))
                    return NULL;
                continue;
            } else if (*p == '<') {
                p++;
                if (*p == '&') {
                    /* N<&M or N<&- */
                    p++;
                    char tgt[16] = {0};
                    int ti = 0;
                    if (*p == '-') { tgt[0] = '-'; ti = 1; p++; }
                    else while (isdigit((unsigned char)*p) && ti < 15) tgt[ti++] = *p++;
                    tgt[ti] = '\0';
                    char enc[64];
                    if (is_named)
                        snprintf(enc, sizeof(enc), "{%s}&%s", fd_name, tgt);
                    else
                        snprintf(enc, sizeof(enc), "%d&%s", fd_num, tgt);
                    if (!add_token(&tokens, &count, &capacity, TOK_REDIR_DUP_IN, strdup(enc)))
                        return NULL;
                    continue;
                }
                /* N<file */
                char enc[64];
                if (is_named)
                    snprintf(enc, sizeof(enc), "{%s}", fd_name);
                else
                    snprintf(enc, sizeof(enc), "%d", fd_num);
                if (!add_token(&tokens, &count, &capacity, TOK_REDIR_FD_IN, strdup(enc)))
                    return NULL;
                continue;
            } else {
                /* not a redirect — backtrack */
                p = fd_start;
            }
        }

        word_parse: ;
        // Words
        const char *start = p;
        /* handle $(...) as single unit — existing logic */
        while (*p) {
            if (isspace(*p)) break;
            if (*p == '|' || *p == '<' || *p == '>' ||
                *p == '&' || *p == ';') break;
            if (*p == '\'' || *p == '"') break;
            if (*p == '$' && *(p+1) == '(') {
                p += 2;
                int depth = 1;
                while (*p && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    p++;
                }
                continue;
            }
            if (*p == '(' || *p == ')') break;
            p++;
        }
        if (p == start) { p++; continue; }

        /* check if word ends with '=' and next char is quote */
        /* e.g. var="hello world" or var='hello world' */
        if (p > start && *(p-1) != '=' && *p != '\'' && *p != '"') {
            /* normal word — no quoted suffix */
            char *value = strdup_range(start, p);
            if (!value) { tokens_free(tokens, count); return NULL; }
            if (!add_token(&tokens, &count, &capacity, TOK_WORD, value))
                return NULL;
            continue;
        }

        /* word ends with = and next is quote, OR word itself is empty
           and we hit a quote — handle quoted suffix */
        if (*p == '\'' || *p == '"') {
            /* read quoted part into a buffer */
            char quote = *p++;
            /* build combined: word_so_far + quoted_content */
            size_t prefix_len = p - start - 1; /* exclude opening quote */
            char combined[MAX_INPUT * 2] = {0};
            memcpy(combined, start, prefix_len);
            size_t ci = prefix_len;

            while (*p && *p != quote) {
                if (*p == '\\' && *(p+1) == quote) {
                    combined[ci++] = quote;
                    p += 2;
                } else {
                    combined[ci++] = *p++;
                }
            }
            if (*p == quote) p++; /* skip closing quote */
            combined[ci] = '\0';

            char *value = strdup(combined);
            if (!value) { tokens_free(tokens, count); return NULL; }
            if (!add_token(&tokens, &count, &capacity, TOK_WORD, value))
                return NULL;
            
            /* If the word contains '=', it's an assignment, not a quoted arg */
            int is_assignment = 0;
            for (size_t j = 0; j < prefix_len; j++) {
                if (combined[j] == '=') {
                    is_assignment = 1;
                    break;
                }
            }
            
            if (!is_assignment) {
                tokens[count-1].quoted = 1;
            }
            continue;
        }

        /* plain word */
        char *value = strdup_range(start, p);
        if (!value) { tokens_free(tokens, count); return NULL; }
        if (!add_token(&tokens, &count, &capacity, TOK_WORD, value))
            return NULL;
    }

    // Add EOF token
    if (!add_token(&tokens, &count, &capacity, TOK_EOF, NULL)) return NULL;

    *ntokens = count;
    return tokens;
}

