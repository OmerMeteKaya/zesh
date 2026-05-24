//
// Created by mete on 23.04.2026.
//

#ifndef SHELL_H
#define SHELL_H

#include <sys/types.h>

#define MAX_ARGS    64
#define MAX_TOKENS  256
#define MAX_INPUT   4096

typedef struct IfNode    IfNode;
typedef struct WhileNode WhileNode;
typedef struct ForNode   ForNode;
typedef struct CmdNode   CmdNode;
typedef struct CmdList   CmdList;

extern volatile __sig_atomic_t g_sigint_received;

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
typedef enum {
    NODE_PIPELINE,
    NODE_IF,
    NODE_WHILE,
    NODE_FOR,
    NODE_FUNC,
    NODE_CASE,
} NodeType;
typedef struct {
    char    *pattern;
    CmdList *body;
} CaseItem;

struct CaseNode {
    char      *word;
    CaseItem  *items;
    int        nitem;
};
typedef struct CaseNode CaseNode;
struct CmdNode {
    Pipeline *pipeline;
    ListOp    op;
    NodeType type;
    union {
        IfNode   *if_node;
        WhileNode *while_node;
        ForNode   *for_node;
        CaseNode  *case_node;
    };
};

struct CmdList {
    CmdNode *nodes;
    int      count;
};



struct IfNode {
    CmdList *condition;
    CmdList *then_body;
    CmdList *elif_conditions[16];
    CmdList *elif_bodies[16];
    int      elif_count;
    CmdList *else_body;
};

struct WhileNode {
    CmdList *condition;
    CmdList *body;
    int      is_until;
};

struct ForNode {
    char    *var;
    char   **words;
    int      nwords;
    CmdList *body;
};

/* functions */
typedef struct {
    char    *name;
    CmdList *body;
} FuncDef;

void positional_set(char **args, int count);
void positional_clear(void);
void        func_define(const char *name, CmdList *body);
FuncDef    *func_get(const char *name);
void        func_free_all(void);
CmdList *func_get_body(const char *name);
/* loop control */
typedef enum {
    LOOP_NORMAL   = 0,
    LOOP_BREAK    = 1,
    LOOP_CONTINUE = 2,
} LoopControl;

extern LoopControl g_loop_control;
extern volatile int g_interrupt_loop;
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

/* return control */
extern int g_return_value;
extern int g_returning;

/* history.c — smart cd */
void  cd_visit(const char *path);
char *cd_frecency_top(const char *query);
char **cd_frecency_list(const char *query, int limit, int *count_out);

/* ---- trap builtin state (signals.c / builtins.c) ---- */
#define TRAP_NSIG 32
extern char *g_trap_actions[TRAP_NSIG]; /* indexed by signal number        */
extern char *g_trap_exit;               /* EXIT (pseudo-signal 0) handler  */
void trap_run_handler(int signum);      /* execute stored trap action       */
void trap_run_exit(int code);           /* run EXIT trap then _exit(code)   */
#endif //SHELL_H
