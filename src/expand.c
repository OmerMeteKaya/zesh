// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#define _GNU_SOURCE
#include "../include/shell.h"
#include "../include/signals.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <glob.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <pwd.h>
#include <errno.h>
#include <signal.h>

#define MAX_ARRAYS     64
#define MAX_ARRAY_SIZE 256
#define MAX_FUNCS 64
int g_current_lineno = 0;
int g_expand_error = 0;
int g_is_subshell = 0;
typedef struct {
    char  name[64];
    char *values[MAX_ARRAY_SIZE];
    int   size;
    int   active;
} LocalArray;

static LocalArray arrays[MAX_ARRAYS];


static pid_t ps_pids[32];
static int   ps_pid_count = 0;
static int   ps_fds[8];
static int   ps_fd_count = 0;



typedef struct {
    char    *name;
    CmdList *body;
    int      active;
} FuncEntry;

static FuncEntry func_table[MAX_FUNCS];

void func_define(const char *name, CmdList *body) {
    for (int i = 0; i < MAX_FUNCS; i++) {
        if (func_table[i].active &&
            strcmp(func_table[i].name, name) == 0) {
            cmdlist_free(func_table[i].body);
            func_table[i].body = body;
            return;
            }
    }
    for (int i = 0; i < MAX_FUNCS; i++) {
        if (!func_table[i].active) {
            func_table[i].name   = strdup(name);
            func_table[i].body   = body;
            func_table[i].active = 1;
            return;
        }
    }
    fprintf(stderr, "zesh: function table full\n");
}

FuncDef *func_get(const char *name) {
    for (int i = 0; i < MAX_FUNCS; i++) {
        if (func_table[i].active &&
            strcmp(func_table[i].name, name) == 0) {
            static FuncDef fd;
            fd.name = func_table[i].name;
            fd.body = func_table[i].body;
            return &fd;
            }
    }
    return NULL;
}
CmdList *func_get_body(const char *name) {
    for (int i = 0; i < MAX_FUNCS; i++) {
        if (func_table[i].active &&
            strcmp(func_table[i].name, name) == 0) {
            CmdList *b = func_table[i].body;
            return b;
            }
    }
    return NULL;
}

void func_free_all(void) {
    for (int i = 0; i < MAX_FUNCS; i++) {
        if (func_table[i].active) {
            free(func_table[i].name);
            cmdlist_free(func_table[i].body);
            func_table[i].active = 0;
        }
    }
}
void ps_pid_register(pid_t pid) {
    if (ps_pid_count < 32) {
        ps_pids[ps_pid_count++] = pid;
    }
}

int ps_pid_forget(pid_t pid) {
    for (int i = 0; i < ps_pid_count; i++) {
        if (ps_pids[i] == pid) {
            // Remove by shifting remaining elements
            for (int j = i; j < ps_pid_count - 1; j++) {
                ps_pids[j] = ps_pids[j + 1];
            }
            ps_pid_count--;
            return 1;
        }
    }
    return 0;
}

void ps_fds_close(void) {
    for (int i = 0; i < ps_fd_count; i++) {
        if (ps_fds[i] >= 0) {
            close(ps_fds[i]);
            ps_fds[i] = -1;
        }
    }
    ps_fd_count = 0;
}

/* Reap all registered process substitution child pids (non-blocking).
 * Called at the end of execute_list() after the main command has exited
 * and pipe read-ends have been closed, so the children will receive
 * EPIPE/SIGPIPE or finish naturally. */
void ps_pids_wait(void) {
    for (int i = 0; i < ps_pid_count; i++) {
        if (ps_pids[i] > 0) {
            int status;
            waitpid(ps_pids[i], &status, WNOHANG);
            /* If still running, leave it — SIGCHLD handler will reap
             * later and ps_pid_forget() will drop it silently. */
        }
    }
    /* Clear the registry regardless; any still-running children remain
     * in the pid list only until the SIGCHLD handler forgets them via
     * ps_pid_forget(). Since we reset the array here, we need a separate
     * shadow list. Simpler: only remove pids that were actually reaped. */
    int remaining = 0;
    for (int i = 0; i < ps_pid_count; i++) {
        if (ps_pids[i] > 0) {
            int status;
            pid_t r = waitpid(ps_pids[i], &status, WNOHANG);
            if (r == 0) {
                /* still alive — keep registered so SIGCHLD can forget it */
                ps_pids[remaining++] = ps_pids[i];
            }
            /* r > 0: reaped; r < 0: gone — drop either way */
        }
    }
    ps_pid_count = remaining;
}

#define MAX_LOCAL_VARS 256
typedef struct {
    char name[64];
    char value[256];
    int  active;
} LocalVar;

static LocalVar local_vars[MAX_LOCAL_VARS];
static int local_var_count = 0;

static LocalArray arrays[MAX_ARRAYS];

/* declared variable attribute table */
VarEntry g_declared_vars[MAX_DECLARED_VARS];
int      g_declared_var_count = 0;

int var_declare(const char *name, const char *value, int attrs) {
    /* update existing entry */
    for (int i = 0; i < g_declared_var_count; i++) {
        if (g_declared_vars[i].active &&
            strcmp(g_declared_vars[i].name, name) == 0) {
            if (g_declared_vars[i].attrs & VAR_ATTR_READONLY)
                return 1;  /* read-only */
            if (value) {
                if (attrs & VAR_ATTR_INTEGER) {
                    /* coerce to integer */
                    long v = atol(value);
                    char ibuf[32];
                    snprintf(ibuf, sizeof(ibuf), "%ld", v);
                    strncpy(g_declared_vars[i].value, ibuf, sizeof(g_declared_vars[i].value)-1);
                } else if (attrs & VAR_ATTR_UPPERCASE) {
                    char *up = strdup(value);
                    if (up) {
                        for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
                        strncpy(g_declared_vars[i].value, up, sizeof(g_declared_vars[i].value)-1);
                        free(up);
                    }
                } else if (attrs & VAR_ATTR_LOWERCASE) {
                    char *lo = strdup(value);
                    if (lo) {
                        for (char *q = lo; *q; q++) *q = (char)tolower((unsigned char)*q);
                        strncpy(g_declared_vars[i].value, lo, sizeof(g_declared_vars[i].value)-1);
                        free(lo);
                    }
                } else {
                    strncpy(g_declared_vars[i].value, value, sizeof(g_declared_vars[i].value)-1);
                }
            }
            g_declared_vars[i].attrs |= attrs;
            if (attrs & VAR_ATTR_EXPORT)
                setenv(name, g_declared_vars[i].value, 1);
            return 0;
        }
    }
    /* new entry */
    if (g_declared_var_count >= MAX_DECLARED_VARS) return 1;
    VarEntry *e = &g_declared_vars[g_declared_var_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, sizeof(e->name)-1);
    const char *cur = var_get(name);
    const char *init_val = value ? value : (cur ? cur : "");
    if (attrs & VAR_ATTR_INTEGER) {
        char *res = eval_arithmetic(init_val);
        if (res) { strncpy(e->value, res, sizeof(e->value)-1); free(res); }
    } else if (attrs & VAR_ATTR_UPPERCASE) {
        char *up = strdup(init_val);
        if (up) {
            for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
            strncpy(e->value, up, sizeof(e->value)-1);
            free(up);
        }
    } else if (attrs & VAR_ATTR_LOWERCASE) {
        char *lo = strdup(init_val);
        if (lo) {
            for (char *q = lo; *q; q++) *q = (char)tolower((unsigned char)*q);
            strncpy(e->value, lo, sizeof(e->value)-1);
            free(lo);
        }
    } else {
        strncpy(e->value, init_val, sizeof(e->value)-1);
    }
    e->attrs  = attrs;
    e->active = 1;
    /* set local var directly (bypass readonly check for initial declaration) */
    for (int i = 0; i < local_var_count; i++) {
        if (local_vars[i].active && strcmp(local_vars[i].name, name) == 0) {
            strncpy(local_vars[i].value, e->value, sizeof(local_vars[i].value)-1);
            goto done_declare;
        }
    }
    if (local_var_count < MAX_LOCAL_VARS) {
        strncpy(local_vars[local_var_count].name, name, sizeof(local_vars[0].name)-1);
        strncpy(local_vars[local_var_count].value, e->value, sizeof(local_vars[0].value)-1);
        local_vars[local_var_count].active = 1;
        local_var_count++;
    }
done_declare:
    if (attrs & VAR_ATTR_EXPORT)
        setenv(name, e->value, 1);
    return 0;
}

int var_get_attrs(const char *name) {
    for (int i = 0; i < g_declared_var_count; i++) {
        if (g_declared_vars[i].active &&
            strcmp(g_declared_vars[i].name, name) == 0)
            return g_declared_vars[i].attrs;
    }
    return 0;
}

void var_list(int attrs_filter) {
    for (int i = 0; i < g_declared_var_count; i++) {
        if (!g_declared_vars[i].active) continue;
        if (attrs_filter && !(g_declared_vars[i].attrs & attrs_filter)) continue;
        char flags[8] = "-";
        int fi = 0;
        if (g_declared_vars[i].attrs & VAR_ATTR_INTEGER)  flags[fi++] = 'i';
        if (g_declared_vars[i].attrs & VAR_ATTR_READONLY) flags[fi++] = 'r';
        if (g_declared_vars[i].attrs & VAR_ATTR_EXPORT)   flags[fi++] = 'x';
        if (g_declared_vars[i].attrs & VAR_ATTR_NAMEREF)  flags[fi++] = 'n';
        flags[fi] = '\0';
        printf("declare -%s %s=\"%s\"\n", fi ? flags : "-",
               g_declared_vars[i].name, g_declared_vars[i].value);
    }
}

/* special variable state */
time_t g_shell_start_time = 0;
char g_current_funcname[256] = {0};
char g_current_source[4096]  = {0};
pid_t g_last_bg_pid = 0;  /* $! */

static const char *get_special_var(const char *name) {
    static char svbuf[64];
    if (strcmp(name, "RANDOM") == 0) {
#ifdef FUZZ_MODE
        snprintf(svbuf, sizeof(svbuf), "%d", 42);
#else
        snprintf(svbuf, sizeof(svbuf), "%d", rand() % 32768);
#endif
        return svbuf;
    }
    if (strcmp(name, "!") == 0) {
        snprintf(svbuf, sizeof(svbuf), "%d", (int)g_last_bg_pid);
        return svbuf;
    }
    if (strcmp(name, "SECONDS") == 0) {
#ifdef FUZZ_MODE
        return "0"; /* FIX: fixed value so $SECONDS is deterministic during fuzzing */
#else
        if (!g_shell_start_time) g_shell_start_time = time(NULL);
        snprintf(svbuf, sizeof(svbuf), "%ld",
                 (long)(time(NULL) - g_shell_start_time));
        return svbuf;
#endif
    }
    if (strcmp(name, "LINENO") == 0) {
        snprintf(svbuf, sizeof(svbuf), "%d", g_current_lineno);
        return svbuf;
    }
    if (strcmp(name, "BASH_SOURCE") == 0) {
        return g_current_source[0] ? g_current_source : "zesh";
    }
    if (strcmp(name, "FUNCNAME") == 0) {
        return g_current_funcname[0] ? g_current_funcname : "";
    }
    return NULL;
}

void arr_set(const char *name, int index, const char *value) {
    if (index < 0 || index >= MAX_ARRAY_SIZE) return;
    /* find existing */
    for (int i = 0; i < MAX_ARRAYS; i++) {
        if (arrays[i].active && strcmp(arrays[i].name, name) == 0) {
            free(arrays[i].values[index]);
            arrays[i].values[index] = value ? strdup(value) : NULL;
            if (index >= arrays[i].size) arrays[i].size = index + 1;
            return;
        }
    }
    /* create new */
    for (int i = 0; i < MAX_ARRAYS; i++) {
        if (!arrays[i].active) {
            memset(&arrays[i], 0, sizeof(LocalArray));
            strncpy(arrays[i].name, name, sizeof(arrays[i].name)-1);
            arrays[i].values[index] = value ? strdup(value) : NULL;
            arrays[i].size = index + 1;
            arrays[i].active = 1;
            return;
        }
    }
}

const char *arr_get(const char *name, int index) {
    if (index < 0 || index >= MAX_ARRAY_SIZE) return NULL;
    for (int i = 0; i < MAX_ARRAYS; i++) {
        if (arrays[i].active && strcmp(arrays[i].name, name) == 0) {
            if (index < arrays[i].size)
                return arrays[i].values[index];
            return NULL;
        }
    }
    return NULL;
}

int arr_len(const char *name) {
    for (int i = 0; i < MAX_ARRAYS; i++) {
        if (arrays[i].active && strcmp(arrays[i].name, name) == 0)
            return arrays[i].size;
    }
    return 0;
}

void arr_set_from_list(const char *name, char **vals, int count) {
    /* clear existing */
    for (int i = 0; i < MAX_ARRAYS; i++) {
        if (arrays[i].active && strcmp(arrays[i].name, name) == 0) {
            for (int j = 0; j < arrays[i].size; j++)
                free(arrays[i].values[j]);
            memset(&arrays[i], 0, sizeof(LocalArray));
            break;
        }
    }
    for (int i = 0; i < count; i++)
        arr_set(name, i, vals[i]);
}

char *expand_process_substitution(const char *cmd_str, int write_mode) {
    if (write_mode) {
        int pipefd[2];
        if (pipe(pipefd) < 0) return NULL;

        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return NULL; }

        if (pid == 0) {
            signals_child();
            close(pipefd[1]);
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);

            extern Token *lex(const char *input, int *ntokens);
            extern CmdList *parse_list(Token *toks, int ntokens);
            extern int execute_list_in_subshell(CmdList *list);
            extern void cmdlist_free(CmdList *list);
            extern void tokens_free(Token *toks, int n);
            extern int last_exit_status;

            int ntokens;
            Token *toks = lex(cmd_str, &ntokens);
            if (toks) {
                CmdList *list = parse_list(toks, ntokens);
                if (list) {
                    execute_list_in_subshell(list);
                    cmdlist_free(list);
                }
                tokens_free(toks, ntokens);
            }
            _exit(0);
        }

        /* parent */
        close(pipefd[0]);
        if (ps_fd_count < 8) ps_fds[ps_fd_count++] = pipefd[1];
        ps_pid_register(pid);

        char *result = malloc(32);
        if (!result) { close(pipefd[1]); return NULL; }
        snprintf(result, 32, "/dev/fd/%d", pipefd[1]);
        return result;
    }

    int pipefd[2];
    if (pipe(pipefd) < 0) return NULL;

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) {
        /* child */
        signals_child();

        /* <(...): child writes stdout to pipe write end */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* run the command */
        extern Token *lex(const char *input, int *ntokens);
        extern Token *glob_expand_tokens(Token *toks, int *ntokens, int last_exit);
        extern Token *word_split_tokens(Token *toks, int ntokens, int *new_count);
        extern CmdList *parse_list(Token *toks, int ntokens);
        extern int execute_list_in_subshell(CmdList *list);
        extern void cmdlist_free(CmdList *list);
        extern void tokens_free(Token *toks, int n);
        extern int last_exit_status;

        int ntokens;
        Token *toks = lex(cmd_str, &ntokens);
        if (toks) {
            toks = glob_expand_tokens(toks, &ntokens, last_exit_status);
            if (toks) {
                toks = word_split_tokens(toks, ntokens, &ntokens);
                if (toks) {
                    CmdList *list = parse_list(toks, ntokens);
                    if (list) {
                        execute_list_in_subshell(list);
                        cmdlist_free(list);
                    }
                    tokens_free(toks, ntokens);
                }
            }
        }
        _exit(0);
    }

    /* parent */
    close(pipefd[1]);  /* write end closed in parent */
    
    /* register fd for later cleanup */
    if (ps_fd_count < 8) {
        ps_fds[ps_fd_count++] = pipefd[0];
    }

    /* register child pid to be ignored in SIGCHLD handler */
    ps_pid_register(pid);

    char *result = malloc(32);
    if (!result) {
        close(pipefd[0]);
        return NULL;
    }
    snprintf(result, 32, "/dev/fd/%d", pipefd[0]);

    return result;  /* caller must eventually close the fd */
}

void local_var_set(const char *name, const char *value) {
    if (!value) value = "";

    /* check if integer-declared: coerce arithmetic */
    const char *store_val = value;
    char arith_buf[64] = {0};
    for (int di = 0; di < g_declared_var_count; di++) {
        if (g_declared_vars[di].active &&
            (g_declared_vars[di].attrs & VAR_ATTR_INTEGER) &&
            strcmp(g_declared_vars[di].name, name) == 0) {
            char *res = eval_arithmetic(value);
            if (res) {
                strncpy(arith_buf, res, sizeof(arith_buf)-1);
                free(res);
                store_val = arith_buf;
            }
            /* update declared var entry too */
            strncpy(g_declared_vars[di].value, store_val,
                    sizeof(g_declared_vars[di].value)-1);
            if (g_declared_vars[di].attrs & VAR_ATTR_EXPORT)
                setenv(name, store_val, 1);
            break;
        }
    }
    /* uppercase/lowercase attribute */
    char case_buf[256] = {0};
    for (int di = 0; di < g_declared_var_count; di++) {
        if (g_declared_vars[di].active &&
            strcmp(g_declared_vars[di].name, name) == 0) {
            if (g_declared_vars[di].attrs & VAR_ATTR_UPPERCASE) {
                strncpy(case_buf, store_val, sizeof(case_buf)-1);
                for (char *q = case_buf; *q; q++) *q = (char)toupper((unsigned char)*q);
                store_val = case_buf;
            } else if (g_declared_vars[di].attrs & VAR_ATTR_LOWERCASE) {
                strncpy(case_buf, store_val, sizeof(case_buf)-1);
                for (char *q = case_buf; *q; q++) *q = (char)tolower((unsigned char)*q);
                store_val = case_buf;
            }
            break;
            }
    }
    /* check readonly */
    for (int di = 0; di < g_declared_var_count; di++) {
        if (g_declared_vars[di].active &&
            (g_declared_vars[di].attrs & VAR_ATTR_READONLY) &&
            strcmp(g_declared_vars[di].name, name) == 0) {
            fprintf(stderr, "zesh: %s: readonly variable\n", name);
            return;
        }
    }

    for (int i = 0; i < local_var_count; i++) {
        if (local_vars[i].active &&
            strcmp(local_vars[i].name, name) == 0) {
            strncpy(local_vars[i].value, store_val,
                    sizeof(local_vars[i].value)-1);
            return;
        }
    }
    if (local_var_count < MAX_LOCAL_VARS) {
        strncpy(local_vars[local_var_count].name, name,
                sizeof(local_vars[0].name)-1);
        strncpy(local_vars[local_var_count].value, store_val,
                sizeof(local_vars[0].value)-1);
        local_vars[local_var_count].active = 1;
        local_var_count++;
    }
}
/* ---- local variable scope stack ---- */
#define MAX_SCOPE_DEPTH 32
typedef struct {
    LocalVar  vars[MAX_LOCAL_VARS];
    int       count;
} ScopeFrame;

static ScopeFrame scope_stack[MAX_SCOPE_DEPTH];
static int        scope_depth = 0;

void scope_push(void) {
    if (scope_depth >= MAX_SCOPE_DEPTH) return;
    ScopeFrame *frame = &scope_stack[scope_depth++];
    frame->count = local_var_count;
    memcpy(frame->vars, local_vars, local_var_count * sizeof(LocalVar));
}

void scope_pop(void) {
    if (scope_depth <= 0) return;
    ScopeFrame *frame = &scope_stack[--scope_depth];
    /* restore: free any string copies if we used heap (we don't here — static bufs) */
    local_var_count = frame->count;
    memcpy(local_vars, frame->vars, local_var_count * sizeof(LocalVar));
}

/* positional parameters — $1 $2 ... $@ $* $# */
#define MAX_POSITIONAL 64
static char *positional_params[MAX_POSITIONAL];
static int   positional_count = 0;

void positional_set(char **args, int count) {
    /* clear existing */
    for (int i = 0; i < positional_count; i++) {
        free(positional_params[i]);
        positional_params[i] = NULL;
    }
    positional_count = count;
    for (int i = 0; i < count && i < MAX_POSITIONAL; i++)
        positional_params[i] = args[i] ? strdup(args[i]) : strdup("");
}

void positional_clear(void) {
    positional_set(NULL, 0);
}

int positional_get_count(void) { return positional_count; }
const char *positional_get(int idx) {
    if (idx < 0 || idx >= positional_count) return "";
    return positional_params[idx] ? positional_params[idx] : "";
}
/* get variable — local first, then env, then special */
const char *var_get(const char *name) {
    /* special variables */
    if (strcmp(name, "RANDOM") == 0 ||
        strcmp(name, "LINENO") == 0 ||
        strcmp(name, "BASH_SOURCE") == 0 ||
        strcmp(name, "FUNCNAME") == 0 ||
        strcmp(name, "SECONDS") == 0 ||
        strcmp(name, "!") == 0) {
        return get_special_var(name);
    }
    /* nameref: follow one level of indirection */
    for (int i = 0; i < g_declared_var_count; i++) {
        if (g_declared_vars[i].active &&
            (g_declared_vars[i].attrs & VAR_ATTR_NAMEREF) &&
            strcmp(g_declared_vars[i].name, name) == 0) {
            /* value holds the name of the target variable */
            return var_get(g_declared_vars[i].value);
        }
    }
    for (int i = 0; i < local_var_count; i++) {
        if (local_vars[i].active &&
            strcmp(local_vars[i].name, name) == 0) {
            return local_vars[i].value;
        }
    }
    /* check declared vars */
    for (int i = 0; i < g_declared_var_count; i++) {
        if (g_declared_vars[i].active &&
            strcmp(g_declared_vars[i].name, name) == 0) {
            return g_declared_vars[i].value;
        }
    }
    const char *env = getenv(name);
    return env;
}

void var_unset(const char *name) {
    unsetenv(name);
    for (int i = 0; i < local_var_count; i++) {
        if (local_vars[i].active &&
            strcmp(local_vars[i].name, name) == 0) {
            local_vars[i].active = 0;
            break;
        }
    }
    for (int i = 0; i < g_declared_var_count; i++) {
        if (g_declared_vars[i].active &&
            strcmp(g_declared_vars[i].name, name) == 0) {
            g_declared_vars[i].active = 0;
            break;
        }
    }
}

static char *itoa(int value) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return strdup(buf);
}

static int is_var_char(int c) {
    return isalnum(c) || c == '_';
}

static int append_str(char **buf, size_t *len, size_t *capacity, const char *str) {
    if (!str) return 0;
    
    size_t str_len = strlen(str);
    if (*len + str_len >= *capacity) {
        size_t new_capacity = *capacity;
        while (new_capacity < *len + str_len + 1) {
            new_capacity *= 2;
        }
        char *tmp = realloc(*buf, new_capacity);
        if (!tmp) return -1;
        *buf = tmp;
        *capacity = new_capacity;
    }
    
    memcpy(*buf + *len, str, str_len);
    *len += str_len;
    return 0;
}

/* Simple arithmetic evaluator — supports +, -, *, /, %, (, ) */
/* Returns result as long, sets *err=1 on error */

typedef struct {
    const char *p;
    int err;
} AEval;

static long ae_expr(AEval *a);   /* forward declaration */

static void ae_skip_ws(AEval *a) {
    while (*a->p == ' ' || *a->p == '\t') a->p++;
}

static long ae_number(AEval *a) {
    ae_skip_ws(a);
    if (*a->p == '(') {
        a->p++;
        long v = ae_expr(a);
        ae_skip_ws(a);
        if (*a->p == ')') a->p++;
        else a->err = 1;
        return v;
    }
    int neg = 0;
    if (*a->p == '-') { neg = 1; a->p++; }
    else if (*a->p == '+') { a->p++; }
    if (!isdigit((unsigned char)*a->p)) { a->err = 1; return 0; }
    long v = 0;
    while (isdigit((unsigned char)*a->p))
        v = v * 10 + (*a->p++ - '0');
    return neg ? -v : v;
}

static long ae_term(AEval *a) {
    long v = ae_number(a);
    while (1) {
        ae_skip_ws(a);
        if (*a->p == '*') { a->p++; v *= ae_number(a); }
        else if (*a->p == '/') {
            a->p++;
            long d = ae_number(a);
            if (d == 0) { a->err = 1; return 0; }
            v /= d;
        }
        else if (*a->p == '%') {
            a->p++;
            long d = ae_number(a);
            if (d == 0) { a->err = 1; return 0; }
            v %= d;
        }
        else break;
    }
    return v;
}

static long ae_expr(AEval *a) {
    long v = ae_term(a);
    while (1) {
        ae_skip_ws(a);
        if      (*a->p == '+') { a->p++; v += ae_term(a); }
        else if (*a->p == '-') { a->p++; v -= ae_term(a); }
        else break;
    }
    return v;
}

static long ae_compare(AEval *a) {
    long v = ae_expr(a);
    while (1) {
        ae_skip_ws(a);
        if (a->p[0]=='=' && a->p[1]=='=') { a->p+=2; v=(v == ae_expr(a)); }
        else if (a->p[0]=='!' && a->p[1]=='=') { a->p+=2; v=(v != ae_expr(a)); }
        else if (a->p[0]=='<' && a->p[1]=='=') { a->p+=2; v=(v <= ae_expr(a)); }
        else if (a->p[0]=='>' && a->p[1]=='=') { a->p+=2; v=(v >= ae_expr(a)); }
        else if (a->p[0]=='<' && a->p[1]!='<') { a->p++;  v=(v <  ae_expr(a)); }
        else if (a->p[0]=='>' && a->p[1]!='>') { a->p++;  v=(v >  ae_expr(a)); }
        else break;
    }
    return v;
}
char *eval_arithmetic(const char *expr) {

    char expanded[1024] = {0};
    const char *p = expr;
    int ei = 0;

    while (*p && ei < 1020) {
        if (*p == '$') {
            p++;
            if (isalpha((unsigned char)*p) || *p == '_') {
                char varname[64] = {0};
                int vi = 0;
                while ((isalnum((unsigned char)*p) || *p == '_') && vi < 63)
                    varname[vi++] = *p++;
                const char *val = var_get(varname);
                if (val) {
                    int vl = strlen(val);
                    if (ei + vl < 1020) {
                        memcpy(expanded + ei, val, vl);
                        ei += vl;
                    }
                } else {
                    /* undefined var → 0 */
                    expanded[ei++] = '0';
                }
            }
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            /* bare identifier without $ — expand from env or local vars */
            char varname[64] = {0};
            int vi = 0;
            while ((isalnum((unsigned char)*p) || *p == '_') && vi < 63)
                varname[vi++] = *p++;
            const char *val = var_get(varname);
            if (val) {
                int vl = strlen(val);
                if (ei + vl < 1020) {
                    memcpy(expanded + ei, val, vl);
                    ei += vl;
                }
            } else {
                /* undefined → 0 */
                expanded[ei++] = '0';
            }
        } else {
            expanded[ei++] = *p++;
        }
    }
    expanded[ei] = '\0';

    AEval a = { expanded, 0 };
    long result = ae_compare(&a);
    if (a.err) return strdup("0");

    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", result);
    return strdup(buf);
}

/* Returns malloc'd array of expanded strings, sets *count */
static char **brace_expand(const char *word, int *count) {
    *count = 0;

    /* find opening brace not preceded by $ */
    const char *open = NULL;
    for (const char *p = word; *p; p++) {
        if (*p == '{' && (p == word || *(p-1) != '$')) {
            open = p;
            break;
        }
    }

    if (!open) {
        /* no brace — return copy of word */
        char **r = malloc(sizeof(char*));
        if (!r) return NULL;
        r[0] = strdup(word);
        *count = 1;
        return r;
    }

    /* find matching close brace */
    const char *close = NULL;
    int depth = 1;
    for (const char *p = open + 1; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) { close = p; break; }
        }
    }

    if (!close) {
        /* unmatched brace — return as-is */
        char **r = malloc(sizeof(char*));
        if (!r) return NULL;
        r[0] = strdup(word);
        *count = 1;
        return r;
    }

    /* prefix: everything before { */
    int prefix_len = open - word;
    if (prefix_len > 1023) prefix_len = 1023;
    char prefix[1024] = {0};
    strncpy(prefix, word, prefix_len);

    /* suffix: everything after } */
    const char *suffix = close + 1;

    /* content between braces */
    int content_len = close - open - 1;
    if (content_len > 1023) content_len = 1023;
    char content[1024] = {0};
    strncpy(content, open + 1, content_len);

    /* check for sequence: {a..z} or {1..5} */
    char seq_from[64] = {0}, seq_to[64] = {0};
    int is_sequence = 0;
    char *dotdot = strstr(content, "..");
    if (dotdot) {
        strncpy(seq_from, content, dotdot - content);
        strncpy(seq_to, dotdot + 2, sizeof(seq_to)-1);
        is_sequence = 1;
    }

    char **items = NULL;
    int item_count = 0;
    int item_cap = 16;
    items = malloc(item_cap * sizeof(char*));
    if (!items) return NULL;

    if (is_sequence) {
        /* numeric sequence */
        int a = atoi(seq_from), b = atoi(seq_to);
        /* char sequence */
        int is_char = (!isdigit((unsigned char)seq_from[0]) &&
                       strlen(seq_from)==1 && strlen(seq_to)==1);
        if (is_char) {
            char ca = seq_from[0], cb = seq_to[0];
            int step = (ca <= cb) ? 1 : -1;
            for (char c = ca; c != cb + step; c += step) {
                if (item_count >= item_cap) {
                    item_cap *= 2;
                    char **tmp = realloc(items, item_cap*sizeof(char*));
                    if (!tmp) break;
                    items = tmp;
                }
                char item[2] = {c, '\0'};
                char full[2048];
                snprintf(full, sizeof(full), "%s%s%s", prefix, item, suffix);
                items[item_count++] = strdup(full);
            }
        } else {
            int step = (a <= b) ? 1 : -1;
            for (int i = a; i != b + step; i += step) {
                if (item_count >= item_cap) {
                    item_cap *= 2;
                    char **tmp = realloc(items, item_cap*sizeof(char*));
                    if (!tmp) break;
                    items = tmp;
                }
                char item[32];
                snprintf(item, sizeof(item), "%d", i);
                char full[2048];
                snprintf(full, sizeof(full), "%s%s%s", prefix, item, suffix);
                items[item_count++] = strdup(full);
            }
        }
    } else {
        /* comma-separated list — split by comma, respecting nested braces */
        char *parts[64];
        int nparts = 0;
        char buf2[1024];
        strncpy(buf2, content, sizeof(buf2)-1);
        char *p2 = buf2;
        char *part_start = p2;
        int d2 = 0;
        while (*p2) {
            if (*p2 == '{') d2++;
            else if (*p2 == '}') d2--;
            else if (*p2 == ',' && d2 == 0) {
                *p2 = '\0';
                if (nparts < 64) parts[nparts++] = part_start;
                part_start = p2 + 1;
            }
            p2++;
        }
        if (nparts < 64) parts[nparts++] = part_start;

        for (int i = 0; i < nparts; i++) {
            if (item_count >= item_cap) {
                item_cap *= 2;
                char **tmp = realloc(items, item_cap*sizeof(char*));
                if (!tmp) break;
                items = tmp;
            }
            char full[2048];
            snprintf(full, sizeof(full), "%s%s%s", prefix, parts[i], suffix);
            items[item_count++] = strdup(full);
        }
    }

    *count = item_count;
    return items;
}

Token *brace_expand_tokens(Token *toks, int *ntokens) {
    if (!toks || *ntokens == 0) return toks;

    /* count how many tokens we'll need after expansion */
    Token *new_toks = malloc(*ntokens * 8 * sizeof(Token));
    if (!new_toks) return toks;
    int new_count = 0;
    int new_cap = *ntokens * 8;

    for (int i = 0; i < *ntokens; i++) {
        if (toks[i].type != TOK_WORD || !toks[i].value) {
            /* non-word token — copy as-is */
            if (new_count >= new_cap) {
                new_cap *= 2;
                Token *tmp = realloc(new_toks, new_cap * sizeof(Token));
                if (!tmp) break;
                new_toks = tmp;
            }
            new_toks[new_count++] = toks[i];
            continue;
        }

        /* check if token contains unquoted brace */
        int has_brace = 0;
        for (const char *p = toks[i].value; *p; p++) {
            if (*p == '{' && (p == toks[i].value || *(p-1) != '$')) {
                has_brace = 1; break;
            }
        }

        if (!has_brace) {
            if (new_count >= new_cap) {
                new_cap *= 2;
                Token *tmp = realloc(new_toks, new_cap * sizeof(Token));
                if (!tmp) break;
                new_toks = tmp;
            }
            new_toks[new_count++] = toks[i];
            continue;
        }

        /* expand braces */
        int exp_count = 0;
        char **expanded = brace_expand(toks[i].value, &exp_count);
        if (!expanded || exp_count == 0) {
            new_toks[new_count++] = toks[i];
            continue;
        }

        /* free original token value */
        free(toks[i].value);

        for (int j = 0; j < exp_count; j++) {
            if (new_count >= new_cap) {
                new_cap *= 2;
                Token *tmp = realloc(new_toks, new_cap * sizeof(Token));
                if (!tmp) { free(expanded[j]); continue; }
                new_toks = tmp;
            }
            new_toks[new_count].type = TOK_WORD;
            new_toks[new_count].value = expanded[j];
            new_toks[new_count].quoted = 0;
            new_count++;
        }
        free(expanded);
    }

    free(toks);
    *ntokens = new_count;
    return new_toks;
}

static char *run_command_substitution(const char *cmd_str) {
    /*
     * Runs cmd_str in a subshell, captures stdout, returns as string.
     * Caller must free() result.
     */
    if (!cmd_str || !*cmd_str) return strdup("");

    /* Create pipe to capture child stdout */
    int pipefd[2];
    if (pipe(pipefd) < 0) return strdup("");
    g_expand_error = 0;
    fflush(NULL);

    /* Block SIGCHLD so the signal handler cannot reap the child before we do */
    sigset_t chldmask, oldmask;
    sigemptyset(&chldmask);
    sigaddset(&chldmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chldmask, &oldmask);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return strdup("");
    }

    if (pid == 0) {
        /* child: unblock SIGCHLD and redirect stdout to pipe write end */
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* run cmd_str through shell pipeline */
        /* re-lex and execute */
        extern Token *lex(const char *input, int *ntokens);
        extern Token *glob_expand_tokens(Token *toks, int *ntokens, int last_exit);
        extern CmdList *parse_list(Token *toks, int ntokens);
        extern int execute_list(CmdList *list);
        extern void cmdlist_free(CmdList *list);
        extern void tokens_free(Token *toks, int n);
        extern int last_exit_status;

        g_expand_error = 0;
        int ntokens;
        Token *toks = lex(cmd_str, &ntokens);
        if (toks) {
            toks = glob_expand_tokens(toks, &ntokens, last_exit_status);
            if (g_expand_error) {
                fflush(stdout);
                tokens_free(toks, ntokens);
                _exit(1);
            }
            if (toks) {
                CmdList *list = parse_list(toks, ntokens);
                if (list) {
                    execute_list(list);
                    cmdlist_free(list);
                }
                tokens_free(toks, ntokens);
            }
        }
        if (g_expand_error) {
            fflush(stdout);
            _exit(1);
        }
        fflush(stdout);
        _exit(last_exit_status);
    }

    /* parent: read from pipe read end */
    close(pipefd[1]);

    char *buf = malloc(4096);
    if (!buf) { close(pipefd[0]); waitpid(pid, NULL, 0); return strdup(""); }
    size_t total = 0, cap = 4096;

    ssize_t n;
    while ((n = read(pipefd[0], buf + total, cap - total - 1)) > 0) {
        total += n;
        if (total + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    if (WIFEXITED(status)) {
        extern int last_exit_status;
        last_exit_status = WEXITSTATUS(status);
        if (WEXITSTATUS(status) != 0) {
            g_expand_error = 1;
        }
    } else if (WIFSIGNALED(status)) {
        g_expand_error = 1;
    }

    buf[total] = '\0';

    /* strip trailing newlines (standard shell behavior) */
    while (total > 0 && buf[total-1] == '\n') {
        buf[--total] = '\0';
    }

    return buf;
}

static char *decode_ansi_c_quoting(const char *str) {
    if (!str || strncmp(str, "$'", 2) != 0) return NULL;

    const char *p = str + 2;
    char *buf = malloc(strlen(str) + 1);
    if (!buf) return NULL;
    size_t bi = 0;

    while (*p && *p != '\'') {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n':  buf[bi++] = '\n'; break;
                case 't':  buf[bi++] = '\t'; break;
                case 'r':  buf[bi++] = '\r'; break;
                case 'a':  buf[bi++] = '\a'; break;
                case 'b':  buf[bi++] = '\b'; break;
                case 'f':  buf[bi++] = '\f'; break;
                case 'v':  buf[bi++] = '\v'; break;
                case 'e':
                case 'E':  buf[bi++] = '\033'; break;
                case '\\': buf[bi++] = '\\'; break;
                case '\'': buf[bi++] = '\''; break;
                case '"':  buf[bi++] = '"'; break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    unsigned val = *p - '0';
                    if (*(p+1) >= '0' && *(p+1) <= '7') { p++; val = val*8 + (*p-'0'); }
                    if (*(p+1) >= '0' && *(p+1) <= '7') { p++; val = val*8 + (*p-'0'); }
                    buf[bi++] = (char)val;
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
                    buf[bi++] = (char)val;
                    break;
                }
                default: buf[bi++] = '\\'; buf[bi++] = *p; break;
            }
            p++;
        } else {
            buf[bi++] = *p++;
        }
    }
    buf[bi] = '\0';

    if (*p == '\'') {
        return buf;
    }
    free(buf);
    return NULL;
}

char *expand_word(const char *word, int last_exit_status) {
    if (!word) return NULL;

    size_t word_len = strlen(word);

    // Handle ANSI-C quoted strings ($'...')
    char *ansi_decoded = decode_ansi_c_quoting(word);
    if (ansi_decoded) {
        return ansi_decoded;
    }

    // Handle single-quoted strings - no expansion
    if (word_len >= 2 && word[0] == '\'' && word[word_len-1] == '\'') {
        return strndup(word + 1, word_len - 2);
    }
    
    char *buf = malloc(256);
    if (!buf) return strdup(word);
    
    size_t len = 0;
    size_t capacity = 256;
    
    const char *p = word;
    
    // Handle double-quoted strings - expand $ only, but keep quotes for processing
    int in_double_quotes = 0;
    if (word_len >= 2 && word[0] == '"' && word[word_len-1] == '"') {
        in_double_quotes = 1;
        p++; // Skip opening quote
        word_len--; // Adjust length
    }
    
    while (*p && (in_double_quotes ? (p < word + word_len) : *p != '\0')) {
        /* process substitution: <(...) or >(...) */
        if ((*p == '<' || *p == '>') && *(p+1) == '(') {
            int write_mode = (*p == '>') ? 1 : 0;
            p += 2;  /* skip <( or >( */
            const char *cmd_start = p;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                p++;
            }
            int cmd_len = (p - 1) - cmd_start;
            if (cmd_len > 0) {
                char *cmd_str = strndup(cmd_start, cmd_len);
                if (cmd_str) {
                    char *fd_path = expand_process_substitution(
                        cmd_str, write_mode);
                    free(cmd_str);
                    if (fd_path) {
                        append_str(&buf, &len, &capacity, fd_path);
                        free(fd_path);
                    }
                }
            }
            continue;
        }

        /* tilde expansion: ~ and ~username */
        if (*p == '~' && (p == word || *(p-1) == '/') && !in_double_quotes) {
            p++;

            /* if username starts with $, expand variable first */
            if (*p == '$') {
                p++;
                const char *vstart = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
                size_t vlen = p - vstart;
                char vname[64] = {0};
                if (vlen > 0 && vlen < 64) strncpy(vname, vstart, vlen);
                const char *vval = var_get(vname);
                const char *uname = vval ? vval : "";
                /* now do passwd lookup with expanded username */
                struct passwd *pw = getpwnam(uname);
                if (pw) {
                    append_str(&buf, &len, &capacity, pw->pw_dir);
                } else {
                    /* unknown — emit ~$varname literally */
                    append_str(&buf, &len, &capacity, "~");
                    append_str(&buf, &len, &capacity, "$");
                    append_str(&buf, &len, &capacity, vname);
                }
                continue;
            }

            /* collect optional username */
            const char *uname_start = p;
            while (*p && *p != '/' && *p != ':' && !isspace((unsigned char)*p) &&
                   *p != '\0') p++;
            size_t uname_len = p - uname_start;

            const char *home_dir = NULL;
            char *pw_buf = NULL;
            if (uname_len == 0) {
                /* bare ~ → $HOME */
                home_dir = getenv("HOME");
                if (!home_dir) home_dir = "";
            } else {
                /* ~username → lookup passwd */
                char uname[256] = {0};
                if (uname_len < sizeof(uname))
                    strncpy(uname, uname_start, uname_len);
                struct passwd *pw = getpwnam(uname);
                if (pw) {
                    pw_buf = strdup(pw->pw_dir);
                    home_dir = pw_buf;
                } else {
                    /* unknown user — emit literal ~username */
                    append_str(&buf, &len, &capacity, "~");
                    char tmp2[256] = {0};
                    strncpy(tmp2, uname_start, uname_len);
                    append_str(&buf, &len, &capacity, tmp2);
                    free(pw_buf);
                    continue;
                }
            }
            if (append_str(&buf, &len, &capacity, home_dir) < 0) {
                free(pw_buf); free(buf);
                return strdup(word);
            }
            free(pw_buf);
            continue;
        }

        // Handle variable expansion
        if (*p == '$') {
            p++;

            /* $1 $2 ... $9 */
            if (*p >= '1' && *p <= '9') {
                int idx = *p - '1';
                p++;
                const char *val = (idx < positional_count) ?
                                   positional_params[idx] : "";
                append_str(&buf, &len, &capacity, val);
                continue;
            }

            /* $# */
            if (*p == '#') {
                p++;
                char nbuf[16];
                snprintf(nbuf, sizeof(nbuf), "%d", positional_count);
                append_str(&buf, &len, &capacity, nbuf);
                continue;
            }

            /* $@ $* */
            if (*p == '@' || *p == '*') {
                p++;
                for (int pi = 0; pi < positional_count; pi++) {
                    if (pi > 0) append_str(&buf, &len, &capacity, " ");
                    append_str(&buf, &len, &capacity,
                               positional_params[pi] ? positional_params[pi] : "");
                }
                continue;
            }

            /* $? */
            if (*p == '?') {
                char *s = itoa(last_exit_status);
                if (s) { append_str(&buf, &len, &capacity, s); free(s); }
                p++;
                continue;
            }

            /* $$ */
            if (*p == '$') {
#ifdef FUZZ_MODE
                /* FIX: fixed 5-digit PID so $$ doesn't cause buffer-realloc non-determinism */
                char *s = strdup("99999");
#else
                char *s = itoa(getpid());
#endif
                if (s) { append_str(&buf, &len, &capacity, s); free(s); }
                p++;
                continue;
            }

            /* $! — last background PID */
            if (*p == '!') {
                char *s = itoa((int)g_last_bg_pid);
                if (s) { append_str(&buf, &len, &capacity, s); free(s); }
                p++;
                continue;
            }
            
            /* $((...)) arithmetic expansion */
            if (*p == '(' && *(p+1) == '(') {
                p += 2;  /* skip (( */
                const char *expr_start = p;
                int depth = 2;
                while (*p && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    p++;
                }
                /* p now points after )) */
                int expr_len = (p - 2) - expr_start;
                if (expr_len > 0) {
                    char *expr = strndup(expr_start, expr_len);
                    if (expr) {
                        char *result = eval_arithmetic(expr);
                        free(expr);
                        if (result) {
                            append_str(&buf, &len, &capacity, result);
                            free(result);
                        }
                    }
                }
                continue;
            }

            // $(...) command substitution
            if (*p == '(') {
                p++;  /* skip '(' */
                const char *cmd_start = p;
                int depth = 1;
                /* find matching closing paren, handle nesting */
                while (*p && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    p++;
                }
                /* p now points after the closing ')' */
                int cmd_len = (p - 1) - cmd_start;  /* exclude closing ')' */
                char *cmd_str = strndup(cmd_start, cmd_len);
                if (cmd_str) {
                    char *output = run_command_substitution(cmd_str);
                    free(cmd_str);
                    if (g_expand_error) {
                        if (output) free(output);
                        free(buf);
                        return NULL;
                    }
                    if (output) {
                        append_str(&buf, &len, &capacity, output);
                        free(output);
                    }
                }
                continue;
            }

            // Handle "${VAR}" - bracketed variable
            if (*p == '{') {
                p++;
                const char *var_start = p;

                /* ${#var} or ${#arr[@]} — length prefix */
                int get_length = 0;
                if (*p == '#' && *(p+1) != '}') {
                    get_length = 1;
                    p++;
                    var_start = p;
                }

                /* read var name — stop at }, [, :, @, % etc. */
                while (*p && *p != '}' && *p != '[' &&
                       *p != ':' && *p != '@' && *p != '%' && *p != '#') p++;

                size_t var_len = p - var_start;
                char var_name[64] = {0};
                if (var_len > 0 && var_len < 64)
                    strncpy(var_name, var_start, var_len);

                /* ${var@Q}, ${var@U}, ${var@L} transforms */
                if (*p == '@') {
                    p++;
                    char op = *p;
                    if (op) p++;
                    if (*p == '}') p++;
                    const char *v = var_get(var_name);
                    if (!v) v = "";
                    if (op == 'Q') {
                        /* single-quote the value */
                        append_str(&buf, &len, &capacity, "'");
                        /* escape single quotes within */
                        for (const char *q = v; *q; q++) {
                            if (*q == '\'') {
                                append_str(&buf, &len, &capacity, "'\\''");
                            } else {
                                char tmp[2] = {*q, 0};
                                append_str(&buf, &len, &capacity, tmp);
                            }
                        }
                        append_str(&buf, &len, &capacity, "'");
                    } else if (op == 'U') {
                        char *upper = strdup(v);
                        if (upper) {
                            for (char *q = upper; *q; q++) *q = (char)toupper((unsigned char)*q);
                            append_str(&buf, &len, &capacity, upper);
                            free(upper);
                        }
                    } else if (op == 'L') {
                        char *lower = strdup(v);
                        if (lower) {
                            for (char *q = lower; *q; q++) *q = (char)tolower((unsigned char)*q);
                            append_str(&buf, &len, &capacity, lower);
                            free(lower);
                        }
                    } else {
                        append_str(&buf, &len, &capacity, v);
                    }
                    continue;
                }

                /* ${var:-default}, ${var:=default}, ${var:+alt}, ${var:?err}
                   also ${var-default} etc. (without colon — only test unset) */
                if (*p == ':' || (*p != '}' && *p != '[' &&
                    (*p == '-' || *p == '=' || *p == '+' || *p == '?'))) {
                    int colon = (*p == ':');
                    if (colon) p++;
                    char op2 = *p;
                    if (op2 == '-' || op2 == '=' || op2 == '+' || op2 == '?') {
                        p++;
                        /* collect the word up to matching } */
                        const char *word_start = p;
                        int brace_depth2 = 1;
                        while (*p && brace_depth2 > 0) {
                            if (*p == '{') brace_depth2++;
                            else if (*p == '}') { brace_depth2--; if (brace_depth2==0) break; }
                            p++;
                        }
                        size_t wlen = p - word_start;
                        char *word_val = strndup(word_start, wlen);
                        if (*p == '}') p++;

                        const char *cur = var_get(var_name);
                        int is_set   = (cur != NULL);
                        int is_empty = (!cur || cur[0] == '\0');
                        int unset_or_empty = colon ? (!is_set || is_empty) : !is_set;

                        char *expanded_word = word_val ? expand_word(word_val, last_exit_status) : strdup("");
                        free(word_val);
                        const char *use = expanded_word ? expanded_word : "";

                        if (op2 == '-') {
                            if (unset_or_empty)
                                append_str(&buf, &len, &capacity, use);
                            else
                                append_str(&buf, &len, &capacity, cur);
                        } else if (op2 == '=') {
                            if (unset_or_empty) {
                                local_var_set(var_name, use);
                                setenv(var_name, use, 1);
                                append_str(&buf, &len, &capacity, use);
                            } else {
                                append_str(&buf, &len, &capacity, cur);
                            }
                        } else if (op2 == '+') {
                            if (!unset_or_empty)
                                append_str(&buf, &len, &capacity, use);
                            /* if unset/empty: expand to nothing */
                        } else if (op2 == '?') {
                            if (unset_or_empty) {
                                fprintf(stderr, "zesh: %s: %s\n", var_name,
                                        *use ? use : "parameter null or not set");
                                free(expanded_word);
                                free(buf);
                                g_expand_error = 1;
                                return NULL;
                            } else {
                                append_str(&buf, &len, &capacity, cur);
                            }
                        }
                        free(expanded_word);
                        continue;
                    }
                    /* colon but not a modifier — treat as plain ${var:} */
                    if (colon && *p == '}') {
                        p++;
                        const char *v = var_get(var_name);
                        if (v) append_str(&buf, &len, &capacity, v);
                        continue;
                    }
                    /* restore p if we consumed ':' but no modifier */
                    if (colon) p--;
                }

                /* substring: ${var:offset:length} */
                if (*p == ':' && (*(p+1) >= '0' && *(p+1) <= '9')) {
                    p++;  /* skip : */
                    /* parse offset */
                    long offset = 0;
                    int neg_off = 0;
                    if (*p == '-') { neg_off=1; p++; }
                    while (*p >= '0' && *p <= '9') offset = offset*10 + (*p++ - '0');
                    long length = -1;
                    if (*p == ':') {
                        p++;
                        length = 0;
                        while (*p >= '0' && *p <= '9') length = length*10 + (*p++ - '0');
                    }
                    if (*p == '}') p++;
                    const char *v = var_get(var_name);
                    if (v) {
                        size_t vlen = strlen(v);
                        if (neg_off) offset = (long)vlen - offset;
                        if (offset < 0) offset = 0;
                        if ((size_t)offset > vlen) offset = vlen;
                        size_t avail = vlen - offset;
                        size_t take = (length < 0) ? avail : (size_t)length;
                        if (take > avail) take = avail;
                        char *sub = strndup(v + offset, take);
                        if (sub) { append_str(&buf, &len, &capacity, sub); free(sub); }
                    }
                    continue;
                }

                /* array index: ${arr[N]} ${arr[@]} ${arr[*]} */
                if (*p == '[') {
                    p++;
                    const char *idx_start = p;
                    while (*p && *p != ']') p++;
                    int idx_len = p - idx_start;
                    char idx_buf[32] = {0};
                    if (idx_len > 0 && idx_len < 32)
                        strncpy(idx_buf, idx_start, idx_len);
                    if (*p == ']') p++;
                    if (*p == '}') p++;

                    if (get_length) {
                        if (strcmp(idx_buf, "@") == 0 || strcmp(idx_buf, "*") == 0) {
                            char nbuf[16];
                            snprintf(nbuf, sizeof(nbuf), "%d", arr_len(var_name));
                            append_str(&buf, &len, &capacity, nbuf);
                        } else {
                            int idx = atoi(idx_buf);
                            const char *v = arr_get(var_name, idx);
                            char nbuf[16];
                            snprintf(nbuf, sizeof(nbuf), "%zu", v ? strlen(v) : 0);
                            append_str(&buf, &len, &capacity, nbuf);
                        }
                    } else if (strcmp(idx_buf, "@") == 0 ||
                               strcmp(idx_buf, "*") == 0) {
                        int alen = arr_len(var_name);
                        for (int ai = 0; ai < alen; ai++) {
                            const char *v = arr_get(var_name, ai);
                            if (!v) v = "";
                            if (ai > 0) append_str(&buf, &len, &capacity, " ");
                            append_str(&buf, &len, &capacity, v);
                        }
                    } else {
                        int idx = atoi(idx_buf);
                        const char *v = arr_get(var_name, idx);
                        if (!v) v = "";
                        append_str(&buf, &len, &capacity, v);
                    }
                    continue;
                }

                /* plain ${VAR} or ${#VAR} */
                if (*p == '}') {
                    p++;
                    if (get_length) {
                        const char *v = var_get(var_name);
                        char nbuf[16];
                        snprintf(nbuf, sizeof(nbuf), "%zu", v ? strlen(v) : (size_t)0);
                        append_str(&buf, &len, &capacity, nbuf);
                    } else {
                        const char *var_value = var_get(var_name);
                        if (!var_value) var_value = "";
                        append_str(&buf, &len, &capacity, var_value);
                    }
                    continue;
                }
                /* malformed — treat literally */
                p = var_start - (get_length ? 1 : 0) - 1;
            }
            
            // Handle "$VAR" - unbracketed variable
            if (is_var_char(*p)) {
                const char *var_start = p;
                while (is_var_char(*p)) p++;
                
                size_t var_len = p - var_start;
                if (var_len > 0) {
                    char *var_name = strndup(var_start, var_len);
                    if (var_name) {
                        const char *var_value = var_get(var_name);
                        if (!var_value) var_value = "";
                        
                        if (append_str(&buf, &len, &capacity, var_value) < 0) {
                            free(var_name);
                            free(buf);
                            return strdup(word);
                        }
                        free(var_name);
                    }
                    continue;
                }
            }
            
            // If we get here, it was just a lone '$'
            if (append_str(&buf, &len, &capacity, "$") < 0) {
                free(buf);
                return strdup(word);
            }
            continue;
        }
        
        // Handle regular characters
        if (len + 2 > capacity) {
            capacity *= 2;
            char *tmp = realloc(buf, capacity);
            if (!tmp) {
                free(buf);
                return strdup(word);
            }
            buf = tmp;
        }
        
        buf[len++] = *p++;
    }
    
    // Add null terminator
    if (len >= capacity) {
        char *tmp = realloc(buf, capacity + 1);
        if (!tmp) {
            free(buf);
            return strdup(word);
        }
        buf = tmp;
    }
    buf[len] = '\0';
    
    return buf;
}



static int has_glob_chars(const char *word) {
    return strpbrk(word, "*?[") != NULL;
}

void expand_tokens(Token *toks, int ntokens, int last_exit_status) {
    if (!toks) return;
    
    for (int i = 0; i < ntokens; i++) {
        if (toks[i].type == TOK_WORD && toks[i].value && !toks[i].quoted) {
            char *expanded = expand_word(toks[i].value, last_exit_status);
            if (expanded) {
                free(toks[i].value);
                toks[i].value = expanded;
            }
        }
    }
}

Token *glob_expand_tokens(Token *toks, int *ntokens, int last_exit_status) {
    if (!toks || !ntokens) return NULL;

    toks = brace_expand_tokens(toks, ntokens);

    expand_tokens(toks, *ntokens, last_exit_status);

    int expanded_count = 0;
    for (int i = 0; i < *ntokens; i++) {
        if (toks[i].type == TOK_WORD && toks[i].value && has_glob_chars(toks[i].value)) {
            glob_t g;
            int ret = glob(toks[i].value, GLOB_NOCHECK|GLOB_TILDE, NULL, &g);
            if (ret == 0 || ret == GLOB_NOMATCH) {
                expanded_count += g.gl_pathc;
                globfree(&g);
            } else {
                expanded_count++;
            }
        } else {
            expanded_count++;
        }
    }

    Token *new_toks = malloc((expanded_count + 1) * sizeof(Token));
    if (!new_toks) return NULL;
    
    int new_index = 0;
    for (int i = 0; i < *ntokens; i++) {
        if (toks[i].type == TOK_WORD && toks[i].value && has_glob_chars(toks[i].value)) {
            glob_t g;
            int ret = glob(toks[i].value, GLOB_NOCHECK|GLOB_TILDE, NULL, &g);
            if (ret == 0 || ret == GLOB_NOMATCH) {
                for (size_t j = 0; j < g.gl_pathc; j++) {
                    new_toks[new_index].type = TOK_WORD;
                    new_toks[new_index].value = strdup(g.gl_pathv[j]);
                    new_toks[new_index].quoted = 0;
                    if (!new_toks[new_index].value) {
                        for (int k = 0; k < new_index; k++) {
                            free(new_toks[k].value);
                        }
                        free(new_toks);
                        globfree(&g);
                        return NULL;
                    }
                    new_index++;
                }
                globfree(&g);
            } else {

                new_toks[new_index].type = toks[i].type;
                new_toks[new_index].value = strdup(toks[i].value);
                new_toks[new_index].quoted = toks[i].quoted;
                if (!new_toks[new_index].value) {
                    for (int k = 0; k < new_index; k++) {
                        free(new_toks[k].value);
                    }
                    free(new_toks);
                    return NULL;
                }
                new_index++;
            }
        } else {

            new_toks[new_index].type = toks[i].type;
            if (toks[i].value) {
                new_toks[new_index].value = strdup(toks[i].value);
                new_toks[new_index].quoted = toks[i].quoted;
                if (!new_toks[new_index].value) {
                    for (int k = 0; k < new_index; k++) {
                        free(new_toks[k].value);
                    }
                    free(new_toks);
                    return NULL;
                }
            } else {
                new_toks[new_index].value = NULL;
            }
            new_index++;
        }
    }

    new_toks[new_index].type = TOK_EOF;
    new_toks[new_index].value = NULL;
    new_toks[new_index].quoted = 0;

    free(toks);
    
    *ntokens = expanded_count;
    return new_toks;
}

Token *word_split_tokens(Token *toks, int ntokens, int *new_count) {
    /*
     * For each TOK_WORD token with quoted=0,
     * split on whitespace after variable expansion.
     * quoted=1 tokens are kept as single tokens (no splitting).
     * Returns new malloc'd token array, frees old one.
     */

    Token *result = malloc(ntokens * 8 * sizeof(Token));
    if (!result) { *new_count = ntokens; return toks; }
    int count = 0;
    int cap = ntokens * 8;

    for (int i = 0; i < ntokens; i++) {
        /* non-word tokens: copy as-is */
        if (toks[i].type != TOK_WORD || !toks[i].value) {
            if (count >= cap) {
                cap *= 2;
                Token *tmp = realloc(result, cap * sizeof(Token));
                if (!tmp) break;
                result = tmp;
            }
            result[count++] = toks[i];
            toks[i].value = NULL; /* ownership transferred */
            continue;
        }

        /* quoted token: no splitting */
        if (toks[i].quoted) {
            if (count >= cap) {
                cap *= 2;
                Token *tmp = realloc(result, cap * sizeof(Token));
                if (!tmp) break;
                result = tmp;
            }
            result[count++] = toks[i];
            toks[i].value = NULL;
            continue;
        }

        /* unquoted token: split on whitespace */
        char *val = toks[i].value;
        char *p = val;
        while (*p) {
            /* skip leading whitespace */
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;

            /* find end of word */
            char *word_start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            int wlen = p - word_start;
            if (wlen == 0) continue;

            if (count >= cap) {
                cap *= 2;
                Token *tmp = realloc(result, cap * sizeof(Token));
                if (!tmp) break;
                result = tmp;
            }
            result[count].type   = TOK_WORD;
            result[count].value  = strndup(word_start, wlen);
            result[count].quoted = 0;
            count++;
        }
        free(toks[i].value);
        toks[i].value = NULL;
    }

    free(toks);
    *new_count = count;
    
    return result;
}
