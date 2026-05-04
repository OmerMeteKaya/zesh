//
// Created by mete on 23.04.2026.
//

#ifndef SHELL_H
#define SHELL_H

#include <sys/types.h>

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
    TOK_SEMI,         /* ;  */
    TOK_HEREDOC,     /* << */
    TOK_HEREDOC_NOEXP, /* <<' (no expansion) */
    TOK_DOUBLE_LBRACKET,  /* [[ */
    TOK_DOUBLE_RBRACKET,  /* ]] */
    TOK_DOUBLE_LPAREN,    /* (( */
    TOK_DOUBLE_RPAREN,    /* )) */
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
    char  *heredoc_content;
    int    heredoc_expand;
    char*  heredoc_delim;/* 1=expand vars, 0=literal */
} Command;

typedef struct {
    Command *commands;
    int      ncommands;
    int      background;
} Pipeline;

typedef enum {
    OP_NONE,
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
int execute_list_in_subshell(CmdList *list);
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
char *expand_process_substitution(const char *cmd_str, int write_mode);
void ps_fds_close(void);
void ps_pids_wait(void);
int  ps_pid_forget(pid_t pid);
char *eval_arithmetic(const char *expr);

/* expand.c — local variables */
void        local_var_set(const char *name, const char *value);
const char *var_get(const char *name);

/* signals.c */
void signals_child(void);

/* here-doc */
void fill_heredocs(CmdList *list);

/* arrays (expand.c) */
void        arr_set(const char *name, int index, const char *value);
const char *arr_get(const char *name, int index);
int         arr_len(const char *name);
void        arr_set_from_list(const char *name, char **vals, int count);

#endif //SHELL_H
