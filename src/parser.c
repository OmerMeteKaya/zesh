//
// Created by mete on 23.04.2026.
//

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

    if (ntokens > 1 && toks[ntokens - 1].type == TOK_BG) {
        pipeline->background = 1;
        ntokens--;
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
            free(in);
        } else if (n->type == NODE_WHILE && n->while_node) {
            WhileNode *wn = n->while_node;
            cmdlist_free(wn->condition);
            cmdlist_free(wn->body);
            free(wn);

        } else if (n->type == NODE_FOR && n->for_node) {
            ForNode *fn = n->for_node;
            free(fn->var);
            for (int w = 0; w < fn->nwords; w++) free(fn->words[w]);
            free(fn->words);
            cmdlist_free(fn->body);
            free(fn);
        } else if (n->type == NODE_CASE && n->case_node) {
            CaseNode *cn = n->case_node;
            free(cn->word);
            for (int ci = 0; ci < cn->nitem; ci++) {
                free(cn->items[ci].pattern);
                cmdlist_free(cn->items[ci].body);
            }
            free(cn->items);
            free(cn);
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
           strcmp(t->value, kw) == 0;
}

/* ------------------------------------------------------------------ */
/*  Token-slice helpers                                                 */
/*  find_keyword: find index of keyword at depth 0, searching forward  */
/*  from 'start'. Returns -1 if not found.                             */
/*  depth tracking: if/while/until/for open a level; fi/done close it  */
/* ------------------------------------------------------------------ */

static int find_closing(Token *toks, int ntokens, int start,
                         const char *open_kw, const char *close_kw) {
    int depth = 1;
    for (int i = start; i < ntokens; i++) {
        if (is_keyword(&toks[i], open_kw))  depth++;
        if (is_keyword(&toks[i], close_kw)) { depth--; if (depth == 0) return i; }
    }
    return -1;
}

/* find a keyword at depth-0, starting from 'start' */
static int find_keyword_d0(Token *toks, int ntokens, int start,
                             const char *kw,
                             const char *open1, const char *close1,
                             const char *open2, const char *close2) {
    int depth = 0;
    for (int i = start; i < ntokens; i++) {
        if (open1  && is_keyword(&toks[i], open1))  depth++;
        if (open2  && is_keyword(&toks[i], open2))  depth++;
        if (close1 && is_keyword(&toks[i], close1)) depth--;
        if (close2 && is_keyword(&toks[i], close2)) depth--;
        if (depth == 0 && is_keyword(&toks[i], kw)) return i;
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
        fprintf(stderr, "mysh: syntax error: expected 'then'\n");
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
            fprintf(stderr, "mysh: syntax error: expected 'fi'\n");
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
                fprintf(stderr, "mysh: syntax error: expected 'fi'\n");
                goto error_if;
            }
            node->else_body = parse_list_internal(toks + i, fi_pos - i);
            i = fi_pos + 1;
            break;
        }

        if (strcmp(found_kw, "elif") == 0) {
            if (node->elif_count >= 16) {
                fprintf(stderr, "mysh: too many elif branches\n");
                goto error_if;
            }
            int ei = node->elif_count;
            i = found + 1; /* skip "elif" */

            /* find "then" for this elif at depth 0 */
            int ethen = find_keyword_d0(toks, ntokens, i,
                                         "then", "if", "fi", NULL, NULL);
            if (ethen < 0) {
                fprintf(stderr, "mysh: syntax error: expected 'then'\n");
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
                fprintf(stderr, "mysh: syntax error: expected 'fi'\n");
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
        fprintf(stderr, "mysh: syntax error: expected 'do'\n");
        free(node); return NULL;
    }
    node->condition = parse_list_internal(toks + i, do_pos - i);
    i = do_pos + 1; /* skip "do" */

    /* find "done" at depth 0 */
    int done_pos = find_closing(toks, ntokens, i,
                                 is_until ? "until" : "while", "done");
    if (done_pos < 0) {
        fprintf(stderr, "mysh: syntax error: expected 'done'\n");
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
        fprintf(stderr, "mysh: syntax error: expected word after 'case'\n");
        free(node); return NULL;
    }
    node->word = strdup(toks[i].value);
    i++;

    /* "in" */
    if (i >= ntokens || !is_keyword(&toks[i], "in")) {
        fprintf(stderr, "mysh: syntax error: expected 'in'\n");
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
        fprintf(stderr, "mysh: syntax error: expected variable after 'for'\n");
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
        fprintf(stderr, "mysh: syntax error: expected 'do'\n");
        goto error_for;
    }
    i++; /* skip "do" */

    /* find "done" */
    int done_pos = find_closing(toks, ntokens, i, "for", "done");
    if (done_pos < 0) {
        fprintf(stderr, "mysh: syntax error: expected 'done'\n");
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

        /* ---- compound commands ---- */
        /* foo() { ... } — function definition */

if (toks[i].type == TOK_WORD && toks[i].value) {
    int fi = i;
    char *fname = NULL;

    size_t vlen = strlen(toks[fi].value);
    if (vlen >= 3 &&
        toks[fi].value[vlen-2] == '(' &&
        toks[fi].value[vlen-1] == ')') {
        fname = strndup(toks[fi].value, vlen - 2);
        fi++;
    }
    else if (fi + 2 < ntokens &&
             toks[fi+1].type == TOK_WORD &&
             toks[fi+1].value &&
             strcmp(toks[fi+1].value, "(") == 0 &&
             toks[fi+2].type == TOK_WORD &&
             toks[fi+2].value &&
             strcmp(toks[fi+2].value, ")") == 0) {
        fname = strdup(toks[fi].value);
        fi += 3;
    }

    if (fname) {
        /* skip optional whitespace/semi, expect { */
        while (fi < ntokens && toks[fi].type == TOK_SEMI) fi++;

        if (fi < ntokens &&
            toks[fi].type == TOK_WORD &&
            toks[fi].value &&
            strcmp(toks[fi].value, "{") == 0) {
            fi++; /* skip { */

            /* find matching } at depth 0 */
            int depth = 1;
            int body_start = fi;
            int body_end   = -1;
            for (int j = fi; j < ntokens; j++) {
                if (toks[j].type == TOK_WORD && toks[j].value) {
                    if (strcmp(toks[j].value, "{") == 0) depth++;
                    else if (strcmp(toks[j].value, "}") == 0) {
                        depth--;
                        if (depth == 0) { body_end = j; break; }
                    }
                }
            }

            if (body_end < 0) {
                fprintf(stderr, "mysh: syntax error: expected '}'\n");
                free(fname);
                goto error;
            }

            CmdList *body = parse_list_internal(
                toks + body_start, body_end - body_start);
            func_define(fname, body);
            free(fname);

            int j = body_end + 1; /* skip } */
            /* check for && || ; after } */
            ListOp op = OP_NONE;
            if (j < ntokens) {
                if      (toks[j].type == TOK_AND)  { op = OP_AND;  j++; }
                else if (toks[j].type == TOK_OR)   { op = OP_OR;   j++; }
                else if (toks[j].type == TOK_SEMI) { op = OP_SEMI; j++; }
            }
            /* func definition node — pipeline=NULL, type=NODE_PIPELINE, op set */
            CmdNode node = {0};
            node.type     = NODE_FUNC;
            node.pipeline = NULL;
            node.op       = op;
            if (!cmdlist_push(list, &capacity, node)) goto error;
            i = j;
            continue;
        }
        free(fname);
    }
}
        if (is_keyword(&toks[i], "if")) {
            int consumed = 0;
            IfNode *in = parse_if(toks + i, ntokens - i, &consumed);
            if (!in) goto error;
            CmdNode node = {0};
            node.type    = NODE_IF;
            node.if_node = in;
            node.op      = OP_NONE;
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
        if (is_keyword(&toks[i], "case")) {
            int consumed = 0;
            CaseNode *cn = parse_case(toks + i, ntokens - i, &consumed);
            if (!cn) goto error;
            CmdNode node = {0};
            node.type      = NODE_CASE;
            node.case_node = cn;
            node.op        = OP_NONE;
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

            /* stop before compound keywords at start of "statement" */
            /* (only if this is the start of a new segment — handled above) */
            i++;
        }

        int seg_len = i - seg_start;
        Pipeline *pl = NULL;
        if (seg_len > 0)
            pl = parse(toks + seg_start, seg_len);

        ListOp op = OP_NONE;
        if (i < ntokens) {
            if      (toks[i].type == TOK_AND)  op = OP_AND;
            else if (toks[i].type == TOK_OR)   op = OP_OR;
            else if (toks[i].type == TOK_SEMI) op = OP_SEMI;
            if (op != OP_NONE) i++;
        }

        if (pl) {
            CmdNode node = {0};
            node.type     = NODE_PIPELINE;
            node.pipeline = pl;
            node.op       = op;
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