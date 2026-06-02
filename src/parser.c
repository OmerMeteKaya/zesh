// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/shell.h"

/* ------------------------------------------------------------------ */
/*  Forward declarations                                                */
/* ------------------------------------------------------------------ */
static CmdList *parse_list_internal(Token *toks, int ntokens);

/* ------------------------------------------------------------------ */
/*  Helpers — argv / command builders (unchanged from original)        */
/* ------------------------------------------------------------------ */

static Command *add_command(Command **commands, int *count, int *capacity) {
    if (*count >= *capacity) {
        *capacity *= 2;
        Command *tmp = realloc(*commands, *capacity * sizeof(Command));
        if (!tmp) return NULL;
        *commands = tmp;
    }
    memset(&(*commands)[*count], 0, sizeof(Command));
    (*commands)[*count].heredoc_expand = 1;
    (*count)++;
    return *commands;
}

static char **add_arg(char ***argv, int *count, int *capacity, char *value) {
    if (*count >= *capacity) {
        *capacity *= 2;
        char **tmp = realloc(*argv, *capacity * sizeof(char *));
        if (!tmp) return NULL;
        *argv = tmp;
    }
    (*argv)[*count] = value;
    (*count)++;
    return *argv;
}

/* ------------------------------------------------------------------ */
/*  parse() — single pipeline (unchanged logic, kept intact)           */
/* ------------------------------------------------------------------ */

Pipeline *parse(Token *toks, int ntokens) {
    if (!toks || ntokens <= 0) return NULL;

    Pipeline *pipeline = calloc(1, sizeof(Pipeline));
    if (!pipeline) return NULL;

    Command *commands = calloc(4, sizeof(Command));
    if (!commands) { free(pipeline); return NULL; }

    int cmd_capacity = 4;
    int cmd_count    = 0;

    Command *current_cmd  = NULL;
    char   **argv         = NULL;
    int      argv_capacity = 8;
    int      argv_count    = 0;

    int i = 0;

    /* check for trailing & (possibly before EOF) */
    if (ntokens > 1 && toks[ntokens - 1].type == TOK_BG) {
        pipeline->background = 1;
        ntokens--;
    } else if (ntokens > 2 &&
               toks[ntokens - 2].type == TOK_BG &&
               toks[ntokens - 1].type == TOK_EOF) {
        pipeline->background = 1;
        /* remove both TOK_BG and TOK_EOF from processing */
        ntokens -= 2;
    }

    if (!add_command(&commands, &cmd_count, &cmd_capacity)) {
        free(commands); free(pipeline); return NULL;
    }
    current_cmd = &commands[cmd_count - 1];
    argv = calloc(argv_capacity, sizeof(char *));
    if (!argv) { free(commands); free(pipeline); return NULL; }

    while (i < ntokens) {
        Token t = toks[i];

        switch (t.type) {

        case TOK_PIPE:
            if (argv_count > 0) {
                argv[argv_count] = NULL;
                current_cmd->argv = argv;
                current_cmd->argc = argv_count;
            } else {
                free(argv);
            }
            argv_capacity = 8; argv_count = 0;
            argv = calloc(argv_capacity, sizeof(char *));
            if (!argv) goto error;
            if (!add_command(&commands, &cmd_count, &cmd_capacity)) {
                free(argv); goto error;
            }
            current_cmd = &commands[cmd_count - 1];
            i++;
            break;

        case TOK_DOUBLE_LBRACKET: {
            char *v = strdup("[[");
            if (!v) goto error;
            if (!add_arg(&argv, &argv_count, &argv_capacity, v)) goto error;
            i++;
            while (i < ntokens &&
                   toks[i].type != TOK_DOUBLE_RBRACKET &&
                   toks[i].type != TOK_EOF) {
                char *word;
                if      (toks[i].type == TOK_AND)  word = strdup("&&");
                else if (toks[i].type == TOK_OR)   word = strdup("||");
                else if (toks[i].type == TOK_PIPE)  word = strdup("|");
                else word = toks[i].value ? strdup(toks[i].value) : strdup("");
                if (!word) goto error;
                if (!add_arg(&argv, &argv_count, &argv_capacity, word)) goto error;
                i++;
            }
            if (i < ntokens && toks[i].type == TOK_DOUBLE_RBRACKET) i++;
            argv[argv_count] = NULL;
            current_cmd->argv = argv;
            current_cmd->argc = argv_count;
            argv = NULL; argv_count = 0;
            break;
        }

        case TOK_DOUBLE_LPAREN: {
            char *v = strdup("((");
            if (!v) goto error;
            if (!add_arg(&argv, &argv_count, &argv_capacity, v)) goto error;
            i++;
            while (i < ntokens &&
                   toks[i].type != TOK_DOUBLE_RPAREN &&
                   toks[i].type != TOK_EOF) {
                char *word = toks[i].value ? strdup(toks[i].value) : strdup("");
                if (!word) goto error;
                if (!add_arg(&argv, &argv_count, &argv_capacity, word)) goto error;
                i++;
            }
            if (i < ntokens && toks[i].type == TOK_DOUBLE_RPAREN) i++;
            argv[argv_count] = NULL;
            current_cmd->argv = argv;
            current_cmd->argc = argv_count;
            argv = NULL; argv_count = 0;
            break;
        }

        case TOK_DOUBLE_RBRACKET:
        case TOK_DOUBLE_RPAREN:
            i++;
            break;

        case TOK_REDIR_IN:
            i++;
            if (i >= ntokens || toks[i].type != TOK_WORD) goto error;
            if (current_cmd->infile) goto error;
            current_cmd->infile = strdup(toks[i].value);
            if (!current_cmd->infile) goto error;
            i++;
            break;

        case TOK_REDIR_OUT:
            i++;
            if (i >= ntokens || toks[i].type != TOK_WORD) goto error;
            if (current_cmd->outfile) goto error;
            current_cmd->outfile = strdup(toks[i].value);
            if (!current_cmd->outfile) goto error;
            current_cmd->append = 0;
            i++;
            break;

        case TOK_REDIR_APP:
            i++;
            if (i >= ntokens || toks[i].type != TOK_WORD) goto error;
            if (current_cmd->outfile) goto error;
            current_cmd->outfile = strdup(toks[i].value);
            if (!current_cmd->outfile) goto error;
            current_cmd->append = 1;
            i++;
            break;

        case TOK_REDIR_FD_OUT:
        case TOK_REDIR_FD_APP:
        case TOK_REDIR_FD_IN: {
            /* value = "N" or "{name}" — file follows */
            if (!t.value || current_cmd->nfd_redirs >= MAX_FD_REDIRS) { i++; break; }
            i++;
            if (i >= ntokens || toks[i].type != TOK_WORD) break;
            FdRedir *r = &current_cmd->fd_redirs[current_cmd->nfd_redirs++];
            /* parse fd number */
            if (t.value[0] == '{') {
                /* named fd — use fd 3+ dynamically; for now store name as is */
                r->src_fd = -1;  /* named — resolved at execution */
            } else {
                r->src_fd = atoi(t.value);
            }
            r->file     = strdup(toks[i].value);
            r->append   = (t.type == TOK_REDIR_FD_APP) ? 1 : 0;
            r->is_input = (t.type == TOK_REDIR_FD_IN)  ? 1 : 0;
            r->dst_fd   = -2; /* -2 = file redir, not dup */
            i++;
            break;
        }

        case TOK_REDIR_DUP_OUT:
        case TOK_REDIR_DUP_IN: {
            /* value = "N" (src fd); target fd/- follows as a word */
            if (!t.value || current_cmd->nfd_redirs >= MAX_FD_REDIRS) { i++; break; }
            i++;
            FdRedir *r = &current_cmd->fd_redirs[current_cmd->nfd_redirs++];
            r->src_fd   = atoi(t.value);  /* 1 for >&, 0 for <& */
            r->is_input = (t.type == TOK_REDIR_DUP_IN) ? 1 : 0;
            r->file     = NULL;
            r->append   = 0;
            /* target: next word or inline number/- */
            if (i < ntokens && toks[i].type == TOK_WORD && toks[i].value) {
                const char *tgt = toks[i].value;
                if (strcmp(tgt, "-") == 0) {
                    r->dst_fd = -1;  /* close */
                } else {
                    /* could be a number or variable — store as file for expansion */
                    /* if purely numeric, use as fd; otherwise store for runtime */
                    r->file = strdup(tgt);
                    r->dst_fd = -2;  /* -2 = "expand file as fd number" */
                }
                i++;
            } else {
                r->dst_fd = -1;  /* nothing following — treat as close */
            }
            break;
        }
            case TOK_HERESTRING: {
                /* <<< word — store as special infile marker */
                if (current_cmd) {
                    char marker[MAX_INPUT];
                    snprintf(marker, sizeof(marker), "\x01HERESTRING\x01%s",
                             t.value ? t.value : "");
                    current_cmd->infile = strdup(marker);
                }
                i++;
                continue;
            }
        case TOK_HEREDOC:
        case TOK_HEREDOC_NOEXP: {
            extern char *read_heredoc(const char *delim, int expand);
            int expand  = (t.type == TOK_HEREDOC) ? 1 : 0;
            char *content = read_heredoc(t.value, expand);
            if (current_cmd) {
                current_cmd->heredoc_content = content;
                current_cmd->heredoc_expand  = expand;
            } else {
                free(content);
            }
            i++;
            break;
        }

        case TOK_WORD:
                if (!add_arg(&argv, &argv_count, &argv_capacity,
                             strdup(t.value))) goto error;
            i++;
            break;

        case TOK_EOF:
            if (argv_count > 0) {
                argv[argv_count] = NULL;
                current_cmd->argv = argv;
                current_cmd->argc = argv_count;
            } else {
                free(argv);
            }
            argv = NULL;
            i++;
            break;

        default:
            goto error;
        }
    }

    /* finalize last command if loop ended without TOK_EOF */
    if (current_cmd && argv_count > 0 && !current_cmd->argv) {
        if (argv_count >= argv_capacity) {
            char **tmp = realloc(argv, (argv_count + 1) * sizeof(char *));
            if (tmp) argv = tmp;
        }
        argv[argv_count] = NULL;
        current_cmd->argv = argv;
        current_cmd->argc = argv_count;
        argv = NULL;
    }
    if (argv) { free(argv); argv = NULL; }

    if (cmd_count == 1 &&
        (!commands[0].argv || commands[0].argc == 0)) {
        free(commands[0].argv);
        free(commands);
        free(pipeline);
        return NULL;
    }

    pipeline->commands  = commands;
    pipeline->ncommands = cmd_count;
    return pipeline;

error:
    if (argv) free(argv);
    for (int j = 0; j < cmd_count; j++) {
        free(commands[j].argv);
        free(commands[j].infile);
        free(commands[j].outfile);
    }
    free(commands);
    free(pipeline);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  pipeline_free                                                       */
/* ------------------------------------------------------------------ */

void pipeline_free(Pipeline *p) {
    if (!p) return;
    for (int i = 0; i < p->ncommands; i++) {
        Command *cmd = &p->commands[i];
        free(cmd->argv);
        free(cmd->infile);
        free(cmd->outfile);
        free(cmd->heredoc_content);
    }
    free(p->commands);
    free(p);
}

/* ------------------------------------------------------------------ */
/*  cmdlist_free  (forward-declared; also frees compound nodes)        */
/* ------------------------------------------------------------------ */

void cmdlist_free(CmdList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        CmdNode *n = &list->nodes[i];
        if (n->type == NODE_PIPELINE) {
            pipeline_free(n->pipeline);
        } else if (n->type == NODE_FUNC) {

        }
        else if (n->type == NODE_IF && n->if_node) {
            IfNode *in = n->if_node;
            cmdlist_free(in->condition);
            cmdlist_free(in->then_body);
            for (int e = 0; e < in->elif_count; e++) {
                cmdlist_free(in->elif_conditions[e]);
                cmdlist_free(in->elif_bodies[e]);
            }
            cmdlist_free(in->else_body);
            free(in->group_outfile);
            free(in->group_infile);
            for (int fi = 0; fi < in->group_nfd_redirs; fi++)
                free(in->group_fd_redirs[fi].file);
            free(in);
        } else if (n->type == NODE_WHILE && n->while_node) {
            WhileNode *wn = n->while_node;
            cmdlist_free(wn->condition);
            cmdlist_free(wn->body);
            free(wn->outfile); free(wn->infile);
            for (int ri = 0; ri < wn->nfd_redirs; ri++) free(wn->fd_redirs[ri].file);
            free(wn);

        } else if (n->type == NODE_FOR && n->for_node) {
            ForNode *fn = n->for_node;
            free(fn->var);
            for (int w = 0; w < fn->nwords; w++) free(fn->words[w]);
            free(fn->words);
            cmdlist_free(fn->body);
            free(fn);
        } else if (n->type == NODE_COPROC && n->coproc_node) {
            pipeline_free(n->coproc_node->pipeline);
            cmdlist_free(n->coproc_node->body);
            free(n->coproc_node->name);
            free(n->coproc_node);
        } else if (n->type == NODE_SELECT && n->select_node) {
            SelectNode *sn = n->select_node;
            free(sn->var);
            for (int w = 0; w < sn->nwords; w++) free(sn->words[w]);
            free(sn->words);
            cmdlist_free(sn->body);
            free(sn->infile); free(sn->outfile);
            for (int ri = 0; ri < sn->nfd_redirs; ri++) free(sn->fd_redirs[ri].file);
            free(sn);
        } else if (n->type == NODE_TIME && n->time_node) {
            pipeline_free(n->time_node->pipeline);
            free(n->time_node);
        } else if (n->type == NODE_CASE && n->case_node) {
            CaseNode *cn = n->case_node;
            free(cn->word);
            for (int ci = 0; ci < cn->nitem; ci++) {
                free(cn->items[ci].pattern);
                cmdlist_free(cn->items[ci].body);
            }
            free(cn->items);
            free(cn);
        } else if (n->type == NODE_SUBSHELL && n->subshell_node) {
            SubshellNode *sn = n->subshell_node;
            cmdlist_free(sn->body);
            free(sn->infile);
            free(sn->outfile);
            for (int fi = 0; fi < sn->nfd_redirs; fi++) free(sn->fd_redirs[fi].file);
            free(sn);
        }

    }
    free(list->nodes);
    free(list);
}

/* ------------------------------------------------------------------ */
/*  CmdList allocation helper                                           */
/* ------------------------------------------------------------------ */

static CmdList *cmdlist_new(void) {
    CmdList *l = malloc(sizeof(CmdList));
    if (!l) return NULL;
    l->nodes    = malloc(4 * sizeof(CmdNode));
    if (!l->nodes) { free(l); return NULL; }
    l->count    = 0;
    return l;
}

/* capacity is stored externally by callers */
static int cmdlist_push(CmdList *list, int *capacity, CmdNode node) {
    if (list->count >= *capacity) {
        *capacity *= 2;
        CmdNode *tmp = realloc(list->nodes, *capacity * sizeof(CmdNode));
        if (!tmp) return 0;
        list->nodes = tmp;
    }
    list->nodes[list->count++] = node;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  is_keyword — returns 1 for if/then/else/elif/fi/while/until/do/    */
/*               done/for/in                                            */
/* ------------------------------------------------------------------ */

static int is_keyword(Token *t, const char *kw) {
    return t->type == TOK_WORD && t->value &&
           !t->quoted &&
           strcmp(t->value, kw) == 0;
}

/* ------------------------------------------------------------------ */
/*  Token-slice helpers                                                 */
/*  find_keyword: find index of keyword at depth 0, searching forward  */
/*  from 'start'. Returns -1 if not found.                             */
/*  depth tracking: if/while/until/for open a level; fi/done close it  */
/* ------------------------------------------------------------------ */

static int compound_delta(Token *t) {
    if (t->type != TOK_WORD || !t->value || t->quoted) return 0;
    const char *v = t->value;
    if (strcmp(v,"if")==0    || strcmp(v,"while")==0  ||
        strcmp(v,"until")==0 || strcmp(v,"for")==0    ||
        strcmp(v,"case")==0  || strcmp(v,"select")==0 ||
        strcmp(v,"{")==0)    return +1;
    if (strcmp(v,"fi")==0  || strcmp(v,"done")==0 ||
        strcmp(v,"esac")==0 || strcmp(v,"}")==0)  return -1;
    return 0;
}

static int find_closing(Token *toks, int ntokens, int start,
                         const char *open_kw, const char *close_kw) {
    (void)open_kw;
    int depth = 1;
    for (int i = start; i < ntokens; i++) {
        depth += compound_delta(&toks[i]);
        if (depth <= 0 && toks[i].type == TOK_WORD &&
            toks[i].value && strcmp(toks[i].value, close_kw) == 0)
            return i;
    }
    return -1;
}

/* find a keyword at depth-0, starting from 'start' */
static int find_keyword_d0(Token *toks, int ntokens, int start,
                             const char *kw,
                             const char *open1, const char *close1,
                             const char *open2, const char *close2) {
    (void)open1; (void)close1; (void)open2; (void)close2;
    int depth = 0;
    for (int i = start; i < ntokens; i++) {
        depth += compound_delta(&toks[i]);
        if (depth == 0 && toks[i].type == TOK_WORD &&
            toks[i].value && strcmp(toks[i].value, kw) == 0)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  parse_if                                                            */
/*                                                                      */
/*  Expects toks[0] == "if"                                             */
/*  Returns IfNode* or NULL; sets *consumed to number of tokens eaten  */
/* ------------------------------------------------------------------ */

static IfNode *parse_if(Token *toks, int ntokens, int *consumed) {
    int i = 1; /* skip "if" */

    IfNode *node = calloc(1, sizeof(IfNode));
    if (!node) return NULL;

    /* condition: tokens until "then" at depth 0 */
    int then_pos = find_keyword_d0(toks, ntokens, i,
                                    "then", "if", "fi", NULL, NULL);
    if (then_pos < 0) {
        fprintf(stderr, "zesh: syntax error: expected 'then'\n");
        free(node); return NULL;
    }
    node->condition = parse_list_internal(toks + i, then_pos - i);
    i = then_pos + 1; /* skip "then" */

    /* then_body parsed flag */
    int then_done = 0;

    while (i < ntokens) {
        /* find next elif/else/fi at depth 0 — clean implementation */
        int found = -1;
        const char *found_kw = NULL;
        int depth = 0;

        for (int j = i; j < ntokens; j++) {
            if (is_keyword(&toks[j], "if")) {
                depth++;
                continue;
            }
            if (is_keyword(&toks[j], "fi")) {
                if (depth > 0) { depth--; continue; }
                /* depth == 0: this fi belongs to us */
                found = j; found_kw = "fi";
                break;
            }
            if (depth > 0) continue; /* inside nested if, skip */
            if (is_keyword(&toks[j], "elif")) { found = j; found_kw = "elif"; break; }
            if (is_keyword(&toks[j], "else")) { found = j; found_kw = "else"; break; }
        }

        if (found < 0) {
            fprintf(stderr, "zesh: syntax error: expected 'fi'\n");
            goto error_if;
        }

        if (!then_done) {
            node->then_body = parse_list_internal(toks + i, found - i);
            then_done = 1;
        }

        if (strcmp(found_kw, "fi") == 0) {
            i = found + 1;
            break;
        }

        if (strcmp(found_kw, "else") == 0) {
            i = found + 1;
            /* find matching fi at depth 0 */
            int depth2 = 0;
            int fi_pos = -1;
            for (int j = i; j < ntokens; j++) {
                if (is_keyword(&toks[j], "if"))  { depth2++; continue; }
                if (is_keyword(&toks[j], "fi")) {
                    if (depth2 > 0) { depth2--; continue; }
                    fi_pos = j; break;
                }
            }
            if (fi_pos < 0) {
                fprintf(stderr, "zesh: syntax error: expected 'fi'\n");
                goto error_if;
            }
            node->else_body = parse_list_internal(toks + i, fi_pos - i);
            i = fi_pos + 1;
            break;
        }

        if (strcmp(found_kw, "elif") == 0) {
            if (node->elif_count >= 16) {
                fprintf(stderr, "zesh: too many elif branches\n");
                goto error_if;
            }
            int ei = node->elif_count;
            i = found + 1; /* skip "elif" */

            /* find "then" for this elif at depth 0 */
            int ethen = find_keyword_d0(toks, ntokens, i,
                                         "then", "if", "fi", NULL, NULL);
            if (ethen < 0) {
                fprintf(stderr, "zesh: syntax error: expected 'then'\n");
                goto error_if;
            }
            node->elif_conditions[ei] = parse_list_internal(toks + i,
                                                              ethen - i);
            i = ethen + 1; /* skip "then" */
            node->elif_count++;

            /* find end of this elif body: next elif/else/fi at depth 0 */
            int found2 = -1;
            const char *fkw2 = NULL;
            int d2 = 0;
            for (int j = i; j < ntokens; j++) {
                if (is_keyword(&toks[j], "if")) { d2++; continue; }
                if (is_keyword(&toks[j], "fi")) {
                    if (d2 > 0) { d2--; continue; }
                    found2 = j; fkw2 = "fi"; break;
                }
                if (d2 > 0) continue;
                if (is_keyword(&toks[j], "elif")) { found2 = j; fkw2 = "elif"; break; }
                if (is_keyword(&toks[j], "else")) { found2 = j; fkw2 = "else"; break; }
            }
            if (found2 < 0) {
                fprintf(stderr, "zesh: syntax error: expected 'fi'\n");
                goto error_if;
            }
            node->elif_bodies[ei] = parse_list_internal(toks + i, found2 - i);
            i = found2;
            (void)fkw2;
            /* outer loop will see elif/else/fi at i and re-process */
            continue;
        }
    }

    *consumed = i;
    return node;

error_if:
    cmdlist_free(node->condition);
    cmdlist_free(node->then_body);
    for (int e = 0; e < node->elif_count; e++) {
        cmdlist_free(node->elif_conditions[e]);
        cmdlist_free(node->elif_bodies[e]);
    }
    cmdlist_free(node->else_body);
    free(node);
    return NULL;
}
/* ------------------------------------------------------------------ */
/*  parse_while / parse_until                                           */
/*                                                                      */
/*  while <cond> do <body> done                                         */
/* ------------------------------------------------------------------ */

static WhileNode *parse_while(Token *toks, int ntokens,
                                int *consumed, int is_until) {
    /* toks[0] = "while"/"until" */
    int i = 1;

    WhileNode *node = calloc(1, sizeof(WhileNode));
    if (!node) return NULL;
    node->is_until = is_until;

    /* find "do" at depth 0 */
    int do_pos = find_keyword_d0(toks, ntokens, i,
                                  "do",
                                  is_until ? "until" : "while", "done",
                                  NULL, NULL);
    if (do_pos < 0) {
        fprintf(stderr, "zesh: syntax error: expected 'do'\n");
        free(node); return NULL;
    }
    node->condition = parse_list_internal(toks + i, do_pos - i);
    i = do_pos + 1; /* skip "do" */

    /* find "done" at depth 0 */
    int done_pos = find_closing(toks, ntokens, i,
                                 is_until ? "until" : "while", "done");
    if (done_pos < 0) {
        fprintf(stderr, "zesh: syntax error: expected 'done'\n");
        cmdlist_free(node->condition);
        free(node); return NULL;
    }
    node->body = parse_list_internal(toks + i, done_pos - i);
    i = done_pos + 1; /* skip "done" */

    *consumed = i;
    return node;
}

/* ------------------------------------------------------------------ */
/*  parse_for                                                           */
/*                                                                      */
/*  for <var> in <words...> ; do <body> done                           */
/*  for <var> in <words...> \n do <body> done   (newline = ;)          */
/* ------------------------------------------------------------------ */
static CaseNode *parse_case(Token *toks, int ntokens, int *consumed) {
    /* toks[0] = "case" */
    int i = 1;

    CaseNode *node = calloc(1, sizeof(CaseNode));
    if (!node) return NULL;

    /* word */
    if (i >= ntokens || !toks[i].value) {
        fprintf(stderr, "zesh: syntax error: expected word after 'case'\n");
        free(node); return NULL;
    }
    node->word = strdup(toks[i].value);
    i++;

    /* "in" */
    if (i >= ntokens || !is_keyword(&toks[i], "in")) {
        fprintf(stderr, "zesh: syntax error: expected 'in'\n");
        free(node->word); free(node); return NULL;
    }
    i++;

    /* items */
    int items_cap = 8;
    node->items = malloc(items_cap * sizeof(CaseItem));
    node->nitem = 0;

    while (i < ntokens) {
        /* skip semicolons */
        while (i < ntokens && toks[i].type == TOK_SEMI) i++;

        /* esac */
        if (i >= ntokens || is_keyword(&toks[i], "esac")) {
            i++;
            break;
        }

        /* pattern — word before ) */
        if (i >= ntokens || !toks[i].value) break;

        char *pattern = strdup(toks[i].value);
        i++;

        /* skip ) */
        if (i < ntokens && toks[i].value &&
            strcmp(toks[i].value, ")") == 0) i++;

        /* body: collect until ;; or esac */
        int body_start = i;
        int body_end   = i;
        int depth      = 0;
        while (body_end < ntokens) {
            if (is_keyword(&toks[body_end], "case")) depth++;
            if (is_keyword(&toks[body_end], "esac")) {
                if (depth > 0) depth--;
                else break;
            }
            /* ;; — double semi */
            if (toks[body_end].type == TOK_SEMI &&
                body_end + 1 < ntokens &&
                toks[body_end + 1].type == TOK_SEMI) break;
            body_end++;
        }

        CmdList *body = parse_list_internal(toks + body_start,
                                             body_end - body_start);

        /* skip ;; */
        if (body_end < ntokens &&
            toks[body_end].type == TOK_SEMI &&
            body_end + 1 < ntokens &&
            toks[body_end + 1].type == TOK_SEMI) {
            body_end += 2;
        }
        i = body_end;

        if (node->nitem >= items_cap) {
            items_cap *= 2;
            CaseItem *tmp = realloc(node->items,
                                     items_cap * sizeof(CaseItem));
            if (!tmp) { free(pattern); cmdlist_free(body); goto error_case; }
            node->items = tmp;
        }
        node->items[node->nitem].pattern = pattern;
        node->items[node->nitem].body    = body;
        node->nitem++;
    }

    *consumed = i;
    return node;

error_case:
    free(node->word);
    for (int ci = 0; ci < node->nitem; ci++) {
        free(node->items[ci].pattern);
        cmdlist_free(node->items[ci].body);
    }
    free(node->items);
    free(node);
    return NULL;
}

static ForNode *parse_for(Token *toks, int ntokens, int *consumed) {
    int i = 1;

    ForNode *node = calloc(1, sizeof(ForNode));
    if (!node) return NULL;

    if (i >= ntokens || toks[i].type != TOK_WORD || !toks[i].value) {
        fprintf(stderr, "zesh: syntax error: expected variable after 'for'\n");
        free(node); return NULL;
    }
    node->var = strdup(toks[i].value);
    i++;

    /* optional "in" */
    int words_cap = 8;
    node->words  = malloc(words_cap * sizeof(char *));
    node->nwords = 0;

    if (i < ntokens && is_keyword(&toks[i], "in")) {
        i++; /* skip "in" */
        /* collect words until ";" or "do" at depth 0 */
        while (i < ntokens &&
               !is_keyword(&toks[i], "do") &&
               toks[i].type != TOK_SEMI) {
            if (node->nwords >= words_cap) {
                words_cap *= 2;
                char **tmp = realloc(node->words, words_cap * sizeof(char *));
                if (!tmp) goto error_for;
                node->words = tmp;
            }
            node->words[node->nwords++] = strdup(toks[i].value);
            i++;
        }
    }
    /* skip optional ";" */
    if (i < ntokens && toks[i].type == TOK_SEMI) i++;

    /* "do" */
    if (i >= ntokens || !is_keyword(&toks[i], "do")) {
        fprintf(stderr, "zesh: syntax error: expected 'do'\n");
        goto error_for;
    }
    i++; /* skip "do" */

    /* find "done" */
    int done_pos = find_closing(toks, ntokens, i, "for", "done");
    if (done_pos < 0) {
        fprintf(stderr, "zesh: syntax error: expected 'done'\n");
        goto error_for;
    }
    node->body = parse_list_internal(toks + i, done_pos - i);
    i = done_pos + 1;

    *consumed = i;
    return node;

error_for:
    free(node->var);
    for (int w = 0; w < node->nwords; w++) free(node->words[w]);
    free(node->words);
    cmdlist_free(node->body);
    free(node);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  parse_select                                                        */
/*  select var in words; do body done                                   */
/* ------------------------------------------------------------------ */
static SelectNode *parse_select(Token *toks, int ntokens, int *consumed) {
    int i = 1;  /* skip "select" */

    SelectNode *node = calloc(1, sizeof(SelectNode));
    if (!node) return NULL;

    if (i >= ntokens || toks[i].type != TOK_WORD || !toks[i].value) {
        free(node); return NULL;
    }
    node->var = strdup(toks[i].value);
    i++;

    int words_cap = 8;
    node->words  = malloc(words_cap * sizeof(char *));
    node->nwords = 0;

    if (i < ntokens && is_keyword(&toks[i], "in")) {
        i++;
        while (i < ntokens && !is_keyword(&toks[i], "do") &&
               toks[i].type != TOK_SEMI) {
            if (node->nwords >= words_cap) {
                words_cap *= 2;
                char **tmp = realloc(node->words, words_cap * sizeof(char *));
                if (!tmp) goto error_sel;
                node->words = tmp;
            }
            node->words[node->nwords++] = strdup(toks[i].value ? toks[i].value : "");
            i++;
        }
    }
    if (i < ntokens && toks[i].type == TOK_SEMI) i++;
    if (i >= ntokens || !is_keyword(&toks[i], "do")) goto error_sel;
    i++;

    int done_pos = find_closing(toks, ntokens, i, "select", "done");
    if (done_pos < 0) goto error_sel;
    node->body = parse_list_internal(toks + i, done_pos - i);
    i = done_pos + 1;
    /* collect redirections after done */
    while (i < ntokens) {
        TokenType rtt = toks[i].type;
        if (rtt == TOK_REDIR_IN || rtt == TOK_REDIR_OUT || rtt == TOK_REDIR_APP ||
            rtt == TOK_REDIR_FD_OUT || rtt == TOK_REDIR_FD_APP || rtt == TOK_REDIR_FD_IN ||
            rtt == TOK_REDIR_DUP_OUT || rtt == TOK_REDIR_DUP_IN) {
            if (rtt == TOK_REDIR_IN && i + 1 < ntokens) {
                node->infile = strdup(toks[i+1].value ? toks[i+1].value : "");
                i += 2;
            } else if (rtt == TOK_REDIR_OUT && i + 1 < ntokens) {
                node->outfile = strdup(toks[i+1].value ? toks[i+1].value : "");
                node->append = 0;
                i += 2;
            } else if (rtt == TOK_REDIR_APP && i + 1 < ntokens) {
                node->outfile = strdup(toks[i+1].value ? toks[i+1].value : "");
                node->append = 1;
                i += 2;
            } else if ((rtt == TOK_REDIR_FD_OUT || rtt == TOK_REDIR_FD_APP ||
                        rtt == TOK_REDIR_FD_IN || rtt == TOK_REDIR_DUP_OUT ||
                        rtt == TOK_REDIR_DUP_IN) && node->nfd_redirs < MAX_FD_REDIRS) {
                FdRedir *r = &node->fd_redirs[node->nfd_redirs++];
                r->src_fd = toks[i].value ? atoi(toks[i].value) : -1;
                r->is_input = (rtt == TOK_REDIR_FD_IN || rtt == TOK_REDIR_DUP_IN);
                r->append = (rtt == TOK_REDIR_FD_APP);
                r->dst_fd = -2; r->file = NULL;
                i++;
                if (i < ntokens && toks[i].type == TOK_WORD && toks[i].value) {
                    if (strcmp(toks[i].value, "-") == 0) { r->dst_fd = -1; }
                    else { r->file = strdup(toks[i].value); }
                    i++;
                }
            } else break;
        } else break;
    }
    *consumed = i;
    return node;

error_sel:
    free(node->var);
    for (int w = 0; w < node->nwords; w++) free(node->words[w]);
    free(node->words);
    free(node);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  parse_list_internal — the real parse_list, used recursively        */
/* ------------------------------------------------------------------ */

static CmdList *parse_list_internal(Token *toks, int ntokens) {
    if (!toks || ntokens <= 0) return NULL;

    CmdList *list     = cmdlist_new();
    if (!list) return NULL;
    int capacity = 4;

    int i = 0;
    while (i < ntokens) {

        /* skip bare semicolons / newlines */
        if (toks[i].type == TOK_SEMI) { i++; continue; }

        /* detect ! negation operator */
        int negate = 0;
        if (toks[i].type == TOK_WORD && toks[i].value &&
            !toks[i].quoted && strcmp(toks[i].value, "!") == 0 &&
            i + 1 < ntokens) {
            negate = 1;
            i++;
        }

        /* ( ) subshell grouping */
        if (toks[i].type == TOK_WORD && toks[i].value && strcmp(toks[i].value, "(") == 0) {
            i++; /* skip ( */
            int depth = 1, body_start = i, body_end = -1;
            for (int j = i; j < ntokens; j++) {
                if (toks[j].type == TOK_WORD && toks[j].value && strcmp(toks[j].value, "(") == 0)
                    depth++;
                if (toks[j].type == TOK_WORD && toks[j].value && strcmp(toks[j].value, ")") == 0) {
                    if (--depth == 0) { body_end = j; break; }
                }
            }
            if (body_end < 0) {
                fprintf(stderr, "zesh: syntax error: expected ')'\n");
                goto error;
            }
            CmdList *sbody = parse_list_internal(toks + body_start,
                                                body_end - body_start);
            SubshellNode *sn = calloc(1, sizeof(SubshellNode));
            if (!sn) { cmdlist_free(sbody); goto error; }
            sn->body = sbody;
            sn->infile = NULL;
            sn->outfile = NULL;
            sn->append = 0;
            sn->nfd_redirs = 0;
            int j = body_end + 1;

            /* collect redirections after ) */
            while (j < ntokens) {
                TokenType rtt = toks[j].type;
                if (rtt == TOK_REDIR_OUT || rtt == TOK_REDIR_APP) {
                    j++;
                    if (j < ntokens && toks[j].value) {
                        free(sn->outfile);
                        sn->outfile = strdup(toks[j].value);
                        sn->append  = (rtt == TOK_REDIR_APP);
                        j++;
                    }
                } else if (rtt == TOK_REDIR_IN) {
                    j++;
                    if (j < ntokens && toks[j].value) {
                        free(sn->infile);
                        sn->infile = strdup(toks[j].value);
                        j++;
                    }
                } else if ((rtt == TOK_REDIR_FD_OUT || rtt == TOK_REDIR_FD_APP ||
                            rtt == TOK_REDIR_FD_IN  || rtt == TOK_REDIR_DUP_OUT ||
                            rtt == TOK_REDIR_DUP_IN) &&
                           sn->nfd_redirs < MAX_FD_REDIRS && toks[j].value) {
                    FdRedir *r = &sn->fd_redirs[sn->nfd_redirs++];
                    r->src_fd   = atoi(toks[j].value);
                    r->is_input = (rtt==TOK_REDIR_DUP_IN||rtt==TOK_REDIR_FD_IN);
                    r->append   = (rtt==TOK_REDIR_FD_APP);
                    r->dst_fd   = -2; r->file = NULL;
                    j++;
                    if (j < ntokens && toks[j].value) {
                        if (strcmp(toks[j].value,"-")==0) r->dst_fd=-1;
                        else { r->file=strdup(toks[j].value); r->dst_fd=-2; }
                        j++;
                    }
                } else break;
            }

            ListOp sop = OP_NONE;
            if (j < ntokens) {
                if      (toks[j].type == TOK_AND)  { sop = OP_AND;  j++; }
                else if (toks[j].type == TOK_OR)   { sop = OP_OR;   j++; }
                else if (toks[j].type == TOK_SEMI) { sop = OP_SEMI; j++; }
                else if (toks[j].type == TOK_PIPE) { sop = OP_PIPE; j++; }
            }
            CmdNode snode = {0};
            snode.type         = NODE_SUBSHELL;
            snode.subshell_node = sn;
            snode.op           = sop;
            snode.negate       = negate;
            if (!cmdlist_push(list, &capacity, snode)) {
                cmdlist_free(sbody); free(sn); goto error;
            }
            i = j;
            continue;
        }

        /* ---- compound commands ---- */
        /* foo() { ... } — function definition */
        if (toks[i].type == TOK_WORD && toks[i].value &&
            strcmp(toks[i].value, "{") != 0 && /* not a bare { group */
            strcmp(toks[i].value, "if") != 0 &&
            strcmp(toks[i].value, "while") != 0 &&
            strcmp(toks[i].value, "for") != 0 &&
            strcmp(toks[i].value, "case") != 0 &&
            strcmp(toks[i].value, "select") != 0 &&
            i + 2 < ntokens &&
            toks[i+1].type == TOK_WORD && toks[i+1].value && strcmp(toks[i+1].value, "(") == 0 &&
            toks[i+2].type == TOK_WORD && toks[i+2].value && strcmp(toks[i+2].value, ")") == 0 &&
            i + 3 < ntokens &&
            toks[i+3].type == TOK_WORD && toks[i+3].value && strcmp(toks[i+3].value, "{") == 0) {
            char *func_name = toks[i].value;
            int body_start = i + 4;
            int depth = 1;
            int body_end = -1;
            for (int j = body_start; j < ntokens; j++) {
                if (toks[j].type == TOK_WORD && toks[j].value) {
                    if (strcmp(toks[j].value, "{") == 0) depth++;
                    else if (strcmp(toks[j].value, "}") == 0) {
                        depth--;
                        if (depth == 0) { body_end = j; break; }
                    }
                }
            }
            if (body_end < 0) {
                fprintf(stderr, "zesh: syntax error: expected '}' in function %s\n", func_name);
                goto error;
            }
            CmdList *fbody = parse_list_internal(toks + body_start, body_end - body_start);
            if (!fbody) goto error;
            func_define(func_name, fbody);
            i = body_end + 1;
            continue;
        }

/* { compound list } — group command */
        if (toks[i].type == TOK_WORD && toks[i].value &&
            strcmp(toks[i].value, "{") == 0) {
            int depth = 1;
            int body_start = i + 1;
            int body_end   = -1;
            for (int j = i + 1; j < ntokens; j++) {
                if (toks[j].type == TOK_WORD && toks[j].value) {
                    if (strcmp(toks[j].value, "{") == 0) depth++;
                    else if (strcmp(toks[j].value, "}") == 0) {
                        depth--;
                        if (depth == 0) { body_end = j; break; }
                    }
                }
            }
            if (body_end < 0) {
                fprintf(stderr, "zesh: syntax error: expected '}'\n");
                goto error;
            }
            CmdList *gbody = parse_list_internal(toks + body_start,
                                                  body_end - body_start);
            int j = body_end + 1;
            IfNode *gn = calloc(1, sizeof(IfNode));
            if (!gn) { cmdlist_free(gbody); goto error; }
            gn->condition  = NULL;
            gn->then_body  = gbody;
            gn->else_body  = NULL;
            gn->elif_count = 0;
            gn->group_nfd_redirs = 0;
            gn->group_outfile = NULL;
            gn->group_infile  = NULL;
            gn->group_append  = 0;
            /* collect redirections following } */
            while (j < ntokens) {
                TokenType rtt = toks[j].type;
                if (rtt == TOK_REDIR_OUT || rtt == TOK_REDIR_APP) {
                    j++;
                    if (j < ntokens && toks[j].value) {
                        free(gn->group_outfile);
                        gn->group_outfile = strdup(toks[j].value);
                        gn->group_append  = (rtt == TOK_REDIR_APP);
                        j++;
                    }
                } else if (rtt == TOK_REDIR_IN) {
                    j++;
                    if (j < ntokens && toks[j].value) {
                        free(gn->group_infile);
                        gn->group_infile = strdup(toks[j].value);
                        j++;
                    }
                } else if ((rtt == TOK_REDIR_FD_OUT || rtt == TOK_REDIR_FD_APP ||
                            rtt == TOK_REDIR_FD_IN  || rtt == TOK_REDIR_DUP_OUT ||
                            rtt == TOK_REDIR_DUP_IN) &&
                           gn->group_nfd_redirs < MAX_FD_REDIRS && toks[j].value) {
                    FdRedir *r = &gn->group_fd_redirs[gn->group_nfd_redirs++];
                    r->src_fd   = atoi(toks[j].value);
                    r->is_input = (rtt==TOK_REDIR_DUP_IN || rtt==TOK_REDIR_FD_IN);
                    r->append   = (rtt==TOK_REDIR_FD_APP);
                    r->dst_fd   = -2;
                    r->file     = NULL;
                    j++;
                    if (j < ntokens && toks[j].value) {
                        if (strcmp(toks[j].value, "-") == 0) {
                            r->dst_fd = -1;
                        } else {
                            r->file   = strdup(toks[j].value);
                            r->dst_fd = -2;
                        }
                        j++;
                    }
                } else {
                    break;
                }
            }
            /* now compute gop */
            ListOp gop = OP_NONE;
            if (j < ntokens) {
                if      (toks[j].type == TOK_AND)  { gop = OP_AND;  j++; }
                else if (toks[j].type == TOK_OR)   { gop = OP_OR;   j++; }
                else if (toks[j].type == TOK_SEMI) { gop = OP_SEMI; j++; }
            }
            CmdNode gnode = {0};
            gnode.type    = NODE_IF;
            gnode.if_node = gn;
            gnode.op      = gop;
            gnode.negate  = negate;
            if (!cmdlist_push(list, &capacity, gnode)) {
                cmdlist_free(gbody); free(gn); goto error;
            }
            i = j;
            continue;
            }
        if (is_keyword(&toks[i], "if")) {
            int consumed = 0;
            IfNode *in = parse_if(toks + i, ntokens - i, &consumed);
            if (!in) goto error;
            CmdNode node = {0};
            node.type    = NODE_IF;
            node.if_node = in;
            node.op      = OP_NONE;
            node.negate  = negate;
            /* check for && || ; after compound */
            int j = i + consumed;
            if (j < ntokens) {
                if      (toks[j].type == TOK_AND)  { node.op = OP_AND;  j++; }
                else if (toks[j].type == TOK_OR)   { node.op = OP_OR;   j++; }
                else if (toks[j].type == TOK_SEMI) { node.op = OP_SEMI; j++; }
            }
            if (!cmdlist_push(list, &capacity, node)) goto error;
            i = j;
            continue;
        }

        if (is_keyword(&toks[i], "while") || is_keyword(&toks[i], "until")) {
            int is_until = is_keyword(&toks[i], "until");
            int consumed = 0;
            WhileNode *wn = parse_while(toks + i, ntokens - i,
                                         &consumed, is_until);
            if (!wn) goto error;
            CmdNode node  = {0};
            node.type     = NODE_WHILE;
            node.while_node = wn;
            node.op       = OP_NONE;
            node.negate   = negate;
            int j = i + consumed;
            /* collect redirections after done */
            while (j < ntokens) {
                TokenType rtt = toks[j].type;
                if (rtt == TOK_REDIR_OUT || rtt == TOK_REDIR_APP) {
                    j++;
                    if (j < ntokens && toks[j].value) {
                        free(wn->outfile);
                        wn->outfile = strdup(toks[j].value);
                        wn->append  = (rtt == TOK_REDIR_APP);
                        j++;
                    }
                } else if (rtt == TOK_REDIR_IN) {
                    j++;
                    if (j < ntokens && toks[j].value) {
                        free(wn->infile); wn->infile = strdup(toks[j].value); j++;
                    }
                } else if ((rtt==TOK_REDIR_FD_OUT||rtt==TOK_REDIR_FD_APP||
                            rtt==TOK_REDIR_FD_IN||rtt==TOK_REDIR_DUP_OUT||
                            rtt==TOK_REDIR_DUP_IN) &&
                           wn->nfd_redirs < MAX_FD_REDIRS && toks[j].value) {
                    FdRedir *r = &wn->fd_redirs[wn->nfd_redirs++];
                    r->src_fd = atoi(toks[j].value);
                    r->is_input = (rtt==TOK_REDIR_DUP_IN||rtt==TOK_REDIR_FD_IN);
                    r->append = (rtt==TOK_REDIR_FD_APP);
                    r->dst_fd = -2; r->file = NULL;
                    j++;
                    if (j < ntokens && toks[j].value) {
                        if (strcmp(toks[j].value,"-")==0) r->dst_fd=-1;
                        else { r->file=strdup(toks[j].value); r->dst_fd=-2; }
                        j++;
                    }
                } else break;
            }
            if (j < ntokens) {
                if      (toks[j].type == TOK_AND)  { node.op = OP_AND;  j++; }
                else if (toks[j].type == TOK_OR)   { node.op = OP_OR;   j++; }
                else if (toks[j].type == TOK_SEMI) { node.op = OP_SEMI; j++; }
                else if (toks[j].type == TOK_PIPE) { node.op = OP_PIPE; j++; }
            }
            if (!cmdlist_push(list, &capacity, node)) goto error;
            i = j;
            continue;
        }
        if (is_keyword(&toks[i], "case")) {
            int consumed = 0;
            CaseNode *cn = parse_case(toks + i, ntokens - i, &consumed);
            if (!cn) goto error;
            CmdNode node = {0};
            node.type      = NODE_CASE;
            node.case_node = cn;
            node.op        = OP_NONE;
            node.negate    = negate;
            int j = i + consumed;
            if (j < ntokens) {
                if      (toks[j].type == TOK_AND)  { node.op = OP_AND;  j++; }
                else if (toks[j].type == TOK_OR)   { node.op = OP_OR;   j++; }
                else if (toks[j].type == TOK_SEMI) { node.op = OP_SEMI; j++; }
            }
            if (!cmdlist_push(list, &capacity, node)) goto error;
            i = j;
            continue;
        }

        if (is_keyword(&toks[i], "for")) {
            int consumed = 0;
            ForNode *fn = parse_for(toks + i, ntokens - i, &consumed);
            if (!fn) goto error;
            CmdNode node = {0};
            node.type    = NODE_FOR;
            node.for_node = fn;
            node.op      = OP_NONE;
            node.negate  = negate;
            int j = i + consumed;
            if (j < ntokens) {
                if      (toks[j].type == TOK_AND)  { node.op = OP_AND;  j++; }
                else if (toks[j].type == TOK_OR)   { node.op = OP_OR;   j++; }
                else if (toks[j].type == TOK_SEMI) { node.op = OP_SEMI; j++; }
            }
            if (!cmdlist_push(list, &capacity, node)) goto error;
            i = j;
            continue;
        }

        if (is_keyword(&toks[i], "select")) {
            int consumed = 0;
            SelectNode *sn = parse_select(toks + i, ntokens - i, &consumed);
            if (!sn) goto error;
            CmdNode node = {0};
            node.type        = NODE_SELECT;
            node.select_node = sn;
            node.op          = OP_NONE;
            node.negate      = negate;
            int j = i + consumed;
            if (j < ntokens) {
                if      (toks[j].type == TOK_AND)  { node.op = OP_AND;  j++; }
                else if (toks[j].type == TOK_OR)   { node.op = OP_OR;   j++; }
                else if (toks[j].type == TOK_SEMI) { node.op = OP_SEMI; j++; }
            }
            if (!cmdlist_push(list, &capacity, node)) goto error;
            i = j;
            continue;
        }

        /* coproc keyword: coproc [NAME] pipeline  or  coproc NAME { ... } */
        if (is_keyword(&toks[i], "coproc")) {
            i++;
            /* optional name: if next token is a word not followed by a redirection */
            char *cp_name = strdup("COPROC");
            if (i < ntokens && toks[i].type == TOK_WORD && toks[i].value &&
                i + 1 < ntokens &&
                toks[i+1].type == TOK_WORD && toks[i+1].value) {
                /* could be NAME + command — use as name */
                free(cp_name);
                cp_name = strdup(toks[i].value);
                i++;
            }
            Pipeline *cp_pl   = NULL;
            CmdList  *cp_body = NULL;
            /* coproc NAME { body } — group command body */
            if (i < ntokens && toks[i].type == TOK_WORD && toks[i].value &&
                strcmp(toks[i].value, "{") == 0) {
                i++; /* skip { */
                int depth2 = 1, body_start = i;
                while (i < ntokens && depth2 > 0) {
                    if (toks[i].type == TOK_WORD && toks[i].value) {
                        if (strcmp(toks[i].value, "{") == 0) depth2++;
                        else if (strcmp(toks[i].value, "}") == 0) depth2--;
                    }
                    if (depth2 > 0) i++;
                    else break;
                }
                int body_end = i;
                if (i < ntokens) i++; /* skip } */
                if (body_end > body_start)
                    cp_body = parse_list(toks + body_start, body_end - body_start);
            } else {
                int cp_start = i;
                while (i < ntokens) {
                    TokenType tt = toks[i].type;
                    if (tt == TOK_AND || tt == TOK_OR || tt == TOK_SEMI) break;
                    i++;
                }
                cp_pl = (i > cp_start) ? parse(toks + cp_start, i - cp_start) : NULL;
            }
            CoprocNode *cn  = calloc(1, sizeof(CoprocNode));
            if (!cn) { free(cp_name); pipeline_free(cp_pl); cmdlist_free(cp_body); goto error; }
            cn->name     = cp_name;
            cn->pipeline = cp_pl;
            cn->body     = cp_body;
            ListOp cp_op = OP_NONE;
            if (i < ntokens) {
                if      (toks[i].type == TOK_AND)  { cp_op = OP_AND;  i++; }
                else if (toks[i].type == TOK_OR)   { cp_op = OP_OR;   i++; }
                else if (toks[i].type == TOK_SEMI) { cp_op = OP_SEMI; i++; }
            }
            CmdNode cnode = {0};
            cnode.type       = NODE_COPROC;
            cnode.coproc_node = cn;
            cnode.op         = cp_op;
            cnode.negate     = negate;
            if (!cmdlist_push(list, &capacity, cnode)) {
                pipeline_free(cp_pl); cmdlist_free(cp_body); free(cp_name); free(cn); goto error;
            }
            continue;
        }

        /* time keyword: time pipeline */
        if (is_keyword(&toks[i], "time")) {
            i++;
            int seg_start2 = i;
            while (i < ntokens) {
                TokenType tt = toks[i].type;
                if (tt == TOK_AND || tt == TOK_OR || tt == TOK_SEMI) break;
                i++;
            }
            int seg_len2 = i - seg_start2;
            Pipeline *tpl = seg_len2 > 0 ? parse(toks + seg_start2, seg_len2) : NULL;
            TimeNode *tn  = calloc(1, sizeof(TimeNode));
            if (!tn) { pipeline_free(tpl); goto error; }
            tn->pipeline = tpl;
            ListOp op2 = OP_NONE;
            if (i < ntokens) {
                if      (toks[i].type == TOK_AND)  { op2 = OP_AND;  i++; }
                else if (toks[i].type == TOK_OR)   { op2 = OP_OR;   i++; }
                else if (toks[i].type == TOK_SEMI) { op2 = OP_SEMI; i++; }
            }
            CmdNode tnode = {0};
            tnode.type      = NODE_TIME;
            tnode.time_node = tn;
            tnode.op        = op2;
            tnode.negate    = negate;
            if (!cmdlist_push(list, &capacity, tnode)) {
                pipeline_free(tpl); free(tn); goto error;
            }
            continue;
        }

        /* ---- plain pipeline ---- */
        /* collect tokens until && || ; EOF at depth 0 */
        int seg_start = i;
        int depth_br  = 0, depth_pr = 0;

        while (i < ntokens) {
            TokenType tt = toks[i].type;
            if (tt == TOK_DOUBLE_LBRACKET) depth_br++;
            else if (tt == TOK_DOUBLE_RBRACKET) depth_br--;
            else if (tt == TOK_DOUBLE_LPAREN)   depth_pr++;
            else if (tt == TOK_DOUBLE_RPAREN)   depth_pr--;

            if (depth_br > 0 || depth_pr > 0) { i++; continue; }

            if (tt == TOK_AND || tt == TOK_OR || tt == TOK_SEMI) break;

            /* stop before compound keywords at depth 0 */
            if (toks[i].type == TOK_WORD && toks[i].value && !toks[i].quoted &&
                depth_br == 0 && depth_pr == 0 &&
                i == seg_start &&
                (strcmp(toks[i].value,"if")==0 || strcmp(toks[i].value,"while")==0 ||
                 strcmp(toks[i].value,"until")==0 || strcmp(toks[i].value,"for")==0 ||
                 strcmp(toks[i].value,"case")==0 || strcmp(toks[i].value,"select")==0 ||
                 strcmp(toks[i].value,"{")==0))
                break;

            /* stop at | when next token is a compound keyword (pipe to compound) */
            if (tt == TOK_PIPE && depth_br == 0 && depth_pr == 0 &&
                i + 1 < ntokens && toks[i+1].type == TOK_WORD &&
                toks[i+1].value && !toks[i+1].quoted) {
                const char *nv = toks[i+1].value;
                if (strcmp(nv,"if")==0    || strcmp(nv,"while")==0  ||
                    strcmp(nv,"until")==0 || strcmp(nv,"for")==0    ||
                    strcmp(nv,"case")==0  || strcmp(nv,"select")==0 ||
                    strcmp(nv,"{")==0)
                    break;
            }

            i++;
        }

        int seg_len = i - seg_start;
        Pipeline *pl = NULL;
        if (seg_len > 0)
            pl = parse(toks + seg_start, seg_len);

        ListOp op = OP_NONE;
        if (i < ntokens) {
            if      (toks[i].type == TOK_AND)  { op = OP_AND;  i++; }
            else if (toks[i].type == TOK_OR)   { op = OP_OR;   i++; }
            else if (toks[i].type == TOK_SEMI) { op = OP_SEMI; i++; }
            else if (toks[i].type == TOK_PIPE) { op = OP_PIPE; i++; }
        }
        if (seg_len == 0 && op == OP_NONE) {
            if (i < ntokens) {
                fprintf(stderr, "zesh: syntax error: unexpected token '%s'\n",
                        toks[i].value ? toks[i].value : "?");
                i++;
            }
            continue;
        }
        if (pl) {
            CmdNode node = {0};
            node.type     = NODE_PIPELINE;
            node.pipeline = pl;
            node.op       = op;
            node.negate   = negate;
            if (!cmdlist_push(list, &capacity, node)) {
                pipeline_free(pl);
                goto error;
            }
        }
    }

    return list;

error:
    cmdlist_free(list);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public parse_list — thin wrapper                                    */
/* ------------------------------------------------------------------ */

CmdList *parse_list(Token *toks, int ntokens) {
    return parse_list_internal(toks, ntokens);
}