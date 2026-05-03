//
// Created by mete on 23.04.2026.
//

#ifndef MYSHELL_SHELL_H
#define MYSHELL_SHELL_H


#define MAX_ARGS    64
#define MAX_TOKENS  256
#define MAX_INPUT   4096

typedef enum {
    TOK_EOF = 0,
    TOK_WORD,
    TOK_PIPE,
    TOK_REDIR_IN,    /* <  */
    TOK_REDIR_OUT,   /* >  */
    TOK_REDIR_APP,   /* >> */
    TOK_BG,          /* &  */
    TOK_AND,         /* && */
    TOK_OR,          /* || */
    TOK_SEMI         /* ;  */
} TokenType;

typedef struct {
    TokenType type;
    char     *value;
    int       quoted;
} Token;

typedef struct {
    char **argv;
    int    argc;
    char  *infile;
    char  *outfile;
    int    append;
} Command;

typedef struct {
    Command *commands;
    int      ncommands;
    int      background;
} Pipeline;

typedef enum {
    OP_NONE,   /* son eleman */
    OP_AND,    /* && */
    OP_OR,     /* || */
    OP_SEMI    /* ;  */
} ListOp;

typedef struct {
    Pipeline *pipeline;
    ListOp    op;
} CmdNode;

typedef struct {
    CmdNode *nodes;
    int      count;
} CmdList;

/* lexer.c */
Token    *lex(const char *input, int *ntokens);
void      tokens_free(Token *toks, int n);

/* parser.c */
Pipeline *parse(Token *toks, int ntokens);
void      pipeline_free(Pipeline *p);
CmdList *parse_list(Token *toks, int ntokens);

/* executor.c */
int execute(Pipeline *p);
int execute_list(CmdList *list);
void cmdlist_free(CmdList *list);

/* builtins.c */
int is_builtin(const char *cmd);
int run_builtin(Command *cmd);

/* expand.c */
char *expand_word(const char *word, int last_exit_status);
void  expand_tokens(Token *toks, int ntokens, int last_exit_status);
Token *glob_expand_tokens(Token *toks, int *ntokens, int last_exit_status);
Token *brace_expand_tokens(Token *toks, int *ntokens);
Token *word_split_tokens(Token *toks, int ntokens, int *new_count);

#endif //MYSHELL_SHELL_H
