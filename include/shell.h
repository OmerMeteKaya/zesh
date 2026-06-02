//
// Created by mete on 23.04.2026.
//

#ifndef SHELL_H
#define SHELL_H

#include <sys/types.h>
#define MAX_ARGS    64
#define MAX_TOKENS  256
#ifdef MAX_INPUT
#undef MAX_INPUT
#endif
#define MAX_INPUT  4096
extern int g_current_lineno;
typedef struct IfNode    IfNode;
typedef struct WhileNode WhileNode;
typedef struct ForNode   ForNode;
typedef struct CmdNode   CmdNode;
typedef struct CmdList   CmdList;

extern volatile __sig_atomic_t g_sigint_received;
extern time_t g_shell_start_time;
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
    TOK_HERESTRING,
    TOK_DOUBLE_LBRACKET,  /* [[ */
    TOK_DOUBLE_RBRACKET,  /* ]] */
    TOK_DOUBLE_LPAREN,    /* (( */
    TOK_DOUBLE_RPAREN,    /* )) */
    TOK_REDIR_FD_OUT,     /* N>  or {name}> */
    TOK_REDIR_FD_APP,     /* N>> or {name}>> */
    TOK_REDIR_FD_IN,      /* N<  or {name}< */
    TOK_REDIR_DUP_OUT,    /* N>&M or N>&- */
    TOK_REDIR_DUP_IN,     /* N<&M or N<&- */
    TOK_REDIR_CLOSE,      /* N>&- or N<&- (close) */
} TokenType;

typedef struct {
    TokenType type;
    char     *value;
    int       quoted;
} Token;

#define MAX_FD_REDIRS 16
typedef struct {
    int   src_fd;     /* the fd to redirect (e.g. 2 for 2>file) */
    int   dst_fd;     /* for dup: the target fd; -1 = close */
    char *file;       /* filename (NULL for dup/close) */
    int   append;
    int   is_input;   /* 1 if input redir */
} FdRedir;

typedef struct {
    char **argv;
    int    argc;
    char  *infile;
    char  *outfile;
    int    append;
    char  *heredoc_content;
    int    heredoc_expand;
    char*  heredoc_delim;/* 1=expand vars, 0=literal */
    FdRedir fd_redirs[MAX_FD_REDIRS];
    int     nfd_redirs;
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
    NODE_SELECT,
    NODE_TIME,
    NODE_COPROC,
    NODE_SUBSHELL,
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
typedef struct {
    char    *var;
    char   **words;
    int      nwords;
    CmdList *body;
} SelectNode;

typedef struct {
    Pipeline *pipeline;  /* the timed pipeline */
} TimeNode;

typedef struct {
    char     *name;      /* optional name, default "COPROC" */
    Pipeline *pipeline;
} CoprocNode;

typedef struct {
    CmdList *body;
} SubshellNode;

struct CmdNode {
    Pipeline *pipeline;
    ListOp    op;
    NodeType type;
    int negate;
    union {
        IfNode     *if_node;
        WhileNode  *while_node;
        ForNode    *for_node;
        CaseNode   *case_node;
        SelectNode *select_node;
        TimeNode   *time_node;
        CoprocNode *coproc_node;
        SubshellNode *subshell_node;
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
    char   *group_outfile;
    char   *group_infile;
    int     group_append;
    FdRedir group_fd_redirs[MAX_FD_REDIRS];
    int     group_nfd_redirs;
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
int         positional_get_count(void);
const char *positional_get(int idx);   /* 0-based */
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
extern int g_expand_error;
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
void        var_unset(const char *name);

/* expand.c — special variable tracking */
extern char  g_current_funcname[256];
extern char  g_current_source[4096];
extern pid_t g_last_bg_pid;  /* $! */

/* declare/typeset variable attributes */
#define VAR_ATTR_READONLY  0x01
#define VAR_ATTR_INTEGER   0x02
#define VAR_ATTR_EXPORT    0x04
#define VAR_ATTR_NAMEREF   0x08
#define VAR_ATTR_LOCAL     0x10
#define VAR_ATTR_UPPERCASE 0x20
#define VAR_ATTR_LOWERCASE 0x40

typedef struct {
    char name[64];
    char value[256];
    int  attrs;   /* bitmask of VAR_ATTR_* */
    int  active;
} VarEntry;

#define MAX_DECLARED_VARS 512
extern VarEntry g_declared_vars[MAX_DECLARED_VARS];
extern int      g_declared_var_count;

int  var_declare(const char *name, const char *value, int attrs);
int  var_get_attrs(const char *name);
void var_list(int attrs_filter);  /* print matching vars */

/* signals.c */
void signals_child(void);

/* here-doc */
void fill_heredocs(CmdList *list);

/* arrays (expand.c) */
void        arr_set(const char *name, int index, const char *value);
const char *arr_get(const char *name, int index);
int         arr_len(const char *name);
void        arr_set_from_list(const char *name, char **vals, int count);

/* local variable scope stack */
void scope_push(void);   /* save current local_vars state on function entry */
void scope_pop(void);    /* restore on function exit */

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
int run_script_line(const char *input);

/* shell options (set -e / -x / -o pipefail) */
extern int g_opt_errexit;   /* set -e  */
extern int g_opt_xtrace;    /* set -x  */
extern int g_opt_pipefail;  /* set -o pipefail */

#endif //SHELL_H
