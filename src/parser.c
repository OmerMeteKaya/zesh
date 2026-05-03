//
// Created by mete on 23.04.2026.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/shell.h"

static Command *add_command(Command **commands, int *count, int *capacity) {
    if (*count >= *capacity) {
        *capacity *= 2;
        Command *tmp = realloc(*commands, *capacity * sizeof(Command));
        if (!tmp) {
            return NULL;
        }
        *commands = tmp;
    }
    memset(&(*commands)[*count], 0, sizeof(Command));
    (*commands)[*count].heredoc_content = NULL;
    (*commands)[*count].heredoc_expand = 1;  /* default to expand */
    (*count)++;
    return *commands;
}

static char **add_arg(char ***argv, int *count, int *capacity, char *value) {
    if (*count >= *capacity) {
        *capacity *= 2;
        char **tmp = realloc(*argv, *capacity * sizeof(char*));
        if (!tmp) {
            return NULL;
        }
        *argv = tmp;
    }
    (*argv)[*count] = value;
    (*count)++;
    return *argv;
}

Pipeline *parse(Token *toks, int ntokens) {
    if (!toks || ntokens <= 0) {
        return NULL;
    }

    Pipeline *pipeline = calloc(1, sizeof(Pipeline));
    if (!pipeline) {
        return NULL;
    }

    Command *commands = calloc(4, sizeof(Command));
    if (!commands) {
        free(pipeline);
        return NULL;
    }

    int cmd_capacity = 4;
    int cmd_count = 0;

    Command *current_cmd = NULL;
    char **argv = NULL;
    int argv_capacity = 8;
    int argv_count = 0;

    int i = 0;
    // Check for background at the end
    if (ntokens > 1 && toks[ntokens-1].type == TOK_BG) {
        pipeline->background = 1;
        ntokens--; // Don't process the BG token as part of commands
    }

    // Add first command
    if (!add_command(&commands, &cmd_count, &cmd_capacity)) {
        free(commands);
        free(pipeline);
        return NULL;
    }
    current_cmd = &commands[cmd_count-1];
    argv = calloc(argv_capacity, sizeof(char*));
    if (!argv) {
        free(commands);
        free(pipeline);
        return NULL;
    }

    while (i < ntokens) {
        Token t = toks[i];

        switch (t.type) {
            case TOK_PIPE:
                // Finish current command
                if (argv_count > 0) {
                    argv[argv_count] = NULL;
                    current_cmd->argv = argv;
                    current_cmd->argc = argv_count;
                } else {
                    free(argv);
                }

                // Start new command
                argv_capacity = 8;
                argv_count = 0;
                argv = calloc(argv_capacity, sizeof(char*));
                if (!argv) {
                    // Clean up already allocated commands
                    for (int j = 0; j < cmd_count; j++) {
                        free(commands[j].argv);
                        free(commands[j].infile);
                        free(commands[j].outfile);
                    }
                    free(commands);
                    free(pipeline);
                    return NULL;
                }

                if (!add_command(&commands, &cmd_count, &cmd_capacity)) {
                    free(argv);
                    // Clean up already allocated commands
                    for (int j = 0; j < cmd_count; j++) {
                        free(commands[j].argv);
                        free(commands[j].infile);
                        free(commands[j].outfile);
                    }
                    free(commands);
                    free(pipeline);
                    return NULL;
                }
                current_cmd = &commands[cmd_count-1];
                i++;
                break;

            case TOK_REDIR_IN:
                i++;
                if (i >= ntokens || toks[i].type != TOK_WORD) {
                    // Error: expected filename after redirection
                    goto error;
                }
                if (current_cmd->infile) {
                    // Error: multiple input redirections
                    goto error;
                }
                current_cmd->infile = strdup(toks[i].value);
                if (!current_cmd->infile) {
                    goto error;
                }
                i++;
                break;

            case TOK_REDIR_OUT:
                i++;
                if (i >= ntokens || toks[i].type != TOK_WORD) {
                    // Error: expected filename after redirection
                    goto error;
                }
                if (current_cmd->outfile) {
                    // Error: multiple output redirections
                    goto error;
                }
                current_cmd->outfile = strdup(toks[i].value);
                if (!current_cmd->outfile) {
                    goto error;
                }
                current_cmd->append = 0;
                i++;
                break;

            case TOK_REDIR_APP:
                i++;
                if (i >= ntokens || toks[i].type != TOK_WORD) {
                    // Error: expected filename after redirection
                    goto error;
                }
                if (current_cmd->outfile) {
                    // Error: multiple output redirections
                    goto error;
                }
                current_cmd->outfile = strdup(toks[i].value);
                if (!current_cmd->outfile) {
                    goto error;
                }
                current_cmd->append = 1;
                i++;
                break;

            case TOK_HEREDOC:
            case TOK_HEREDOC_NOEXP:
                /* delimiter is in t.value */
                /* read here-doc content now — blocking read from stdin */
                {
                    extern char *read_heredoc(const char *delim, int expand);
                    int expand = (t.type == TOK_HEREDOC) ? 1 : 0;
                    char *content = read_heredoc(t.value, expand);
                    if (current_cmd) {
                        current_cmd->heredoc_content = content;
                        current_cmd->heredoc_expand = (t.type == TOK_HEREDOC) ? 1 : 0;
                    } else {
                        free(content);
                    }
                }
                i++;
                break;

            case TOK_WORD:
                if (!add_arg(&argv, &argv_count, &argv_capacity, t.value)) {
                    goto error;
                }
                i++;
                break;

            case TOK_EOF:
                // Finish current command
                if (argv_count > 0) {
                    argv[argv_count] = NULL;
                    current_cmd->argv = argv;
                    current_cmd->argc = argv_count;
                } else {
                    free(argv);
                }
                i++;
                break;

            default:
                // Unknown token type
                goto error;
        }
    }

    /* Finalize last command if loop ended without TOK_EOF */
    if (current_cmd && argv_count > 0 && !current_cmd->argv) {
        if (argv_count >= argv_capacity) {
            char **tmp = realloc(argv, (argv_count + 1) * sizeof(char*));
            if (tmp) argv = tmp;
        }
        argv[argv_count] = NULL;
        current_cmd->argv = argv;
        current_cmd->argc = argv_count;
    } else if (argv) {
        free(argv);
    }

    // Handle case where there were no tokens except maybe BG
    if (cmd_count == 1 && (!commands[0].argv || commands[0].argc == 0)) {
        if (commands[0].argv) {
            free(commands[0].argv);
        }
        free(commands);
        free(pipeline);
        return NULL;
    }

    pipeline->commands = commands;
    pipeline->ncommands = cmd_count;
    return pipeline;

error:
    // Clean up on error
    if (argv) {
        free(argv);
    }
    for (int j = 0; j < cmd_count; j++) {
        if (commands[j].argv) {
            free(commands[j].argv);
        }
        if (commands[j].infile) {
            free(commands[j].infile);
        }
        if (commands[j].outfile) {
            free(commands[j].outfile);
        }
    }
    free(commands);
    free(pipeline);
    return NULL;
}

void pipeline_free(Pipeline *p) {
    if (!p) return;

    for (int i = 0; i < p->ncommands; i++) {
        Command *cmd = &p->commands[i];
        if (cmd->argv) {
            free(cmd->argv);
        }
        if (cmd->infile) {
            free(cmd->infile);
        }
        if (cmd->outfile) {
            free(cmd->outfile);
        }
        if (cmd->heredoc_content) {
            free(cmd->heredoc_content);
            cmd->heredoc_content = NULL;
        }
    }
    free(p->commands);
    free(p);
}

CmdList *parse_list(Token *toks, int ntokens) {
    if (!toks || ntokens <= 0) {
        return NULL;
    }

    CmdList *list = malloc(sizeof(CmdList));
    if (!list) {
        return NULL;
    }

    list->nodes = malloc(4 * sizeof(CmdNode));
    if (!list->nodes) {
        free(list);
        return NULL;
    }

    int capacity = 4;
    int count = 0;
    int segment_start = 0;

    for (int i = 0; i <= ntokens; i++) {
        TokenType type = (i == ntokens) ? TOK_EOF : toks[i].type;

        if (type == TOK_AND || type == TOK_OR || type == TOK_SEMI || type == TOK_EOF) {
            // Segmenti ayrıştır
            Pipeline *pipeline = NULL;
            int segment_len = i - segment_start;
            
            if (segment_len > 0) {
                pipeline = parse(toks + segment_start, segment_len);
                // Boş segmentler için placeholder oluştur - sessizce devam et
                if (!pipeline) {
                    // Boş pipeline için NULL bırakılabilir
                }
            }

            // CmdNode oluştur ve listeye ekle
            if (count >= capacity) {
                capacity *= 2;
                CmdNode *tmp = realloc(list->nodes, capacity * sizeof(CmdNode));
                if (!tmp) {
                    // Temizlik yap
                    if (pipeline) {
                        pipeline_free(pipeline);
                    }
                    for (int j = 0; j < count; j++) {
                        if (list->nodes[j].pipeline) {
                            pipeline_free(list->nodes[j].pipeline);
                        }
                    }
                    free(list->nodes);
                    free(list);
                    return NULL;
                }
                list->nodes = tmp;
            }

            ListOp op = OP_NONE;
            if (type == TOK_AND) op = OP_AND;
            else if (type == TOK_OR) op = OP_OR;
            else if (type == TOK_SEMI) op = OP_SEMI;

            list->nodes[count].pipeline = pipeline;
            list->nodes[count].op = op;
            count++;

            segment_start = i + 1;
        }
    }

    list->count = count;
    return list;
}

void cmdlist_free(CmdList *list) {
    if (!list) return;

    for (int i = 0; i < list->count; i++) {
        if (list->nodes[i].pipeline) {
            pipeline_free(list->nodes[i].pipeline);
        }
    }
    free(list->nodes);
    free(list);
}
