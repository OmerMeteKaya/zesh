//
// Created by mete on 23.04.2026.
//

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
                if (*(p+1) == '<') {
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
            const char *start = ++p;
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
        word:
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

