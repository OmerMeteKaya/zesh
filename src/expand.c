//
// Created by mete on 26.04.2026.
//

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

#define MAX_ARRAYS     64
#define MAX_ARRAY_SIZE 256
#define MAX_FUNCS 64

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
    fprintf(stderr, "mysh: function table full\n");
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
    for (int i = 0; i < local_var_count; i++) {
        if (local_vars[i].active &&
            strcmp(local_vars[i].name, name) == 0) {
            strncpy(local_vars[i].value, value,
                    sizeof(local_vars[i].value)-1);
            return;
            }
    }
    if (local_var_count < MAX_LOCAL_VARS) {
        strncpy(local_vars[local_var_count].name, name,
                sizeof(local_vars[0].name)-1);
        strncpy(local_vars[local_var_count].value, value,
                sizeof(local_vars[0].value)-1);
        local_vars[local_var_count].active = 1;
        local_var_count++;
    }
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
/* get variable — local first, then env */
const char *var_get(const char *name) {
    for (int i = 0; i < local_var_count; i++) {
        if (local_vars[i].active &&
            strcmp(local_vars[i].name, name) == 0) {
            return local_vars[i].value;
        }
    }
    const char *env = getenv(name);
    return env;
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
    char prefix[1024] = {0};
    strncpy(prefix, word, prefix_len);

    /* suffix: everything after } */
    const char *suffix = close + 1;

    /* content between braces */
    int content_len = close - open - 1;
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

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return strdup("");
    }

    if (pid == 0) {
        /* child: redirect stdout to pipe write end */
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

        int ntokens;
        Token *toks = lex(cmd_str, &ntokens);
        if (toks) {
            toks = glob_expand_tokens(toks, &ntokens, last_exit_status);
            if (toks) {
                CmdList *list = parse_list(toks, ntokens);
                if (list) {
                    execute_list(list);
                    cmdlist_free(list);
                }
                tokens_free(toks, ntokens);
            }
        }
        _exit(0);
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
    waitpid(pid, NULL, 0);

    buf[total] = '\0';

    /* strip trailing newlines (standard shell behavior) */
    while (total > 0 && buf[total-1] == '\n') {
        buf[--total] = '\0';
    }

    return buf;
}

char *expand_word(const char *word, int last_exit_status) {
    if (!word) return NULL;
    
    size_t word_len = strlen(word);
    
    // Handle single-quoted strings - no expansion
    if (word_len >= 2 && word[0] == '\'' && word[word_len-1] == '\'') {
        return strdup(word);
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

        // Handle tilde expansion (only at start of word or after slash, and not in double quotes for mid-word)
        if (*p == '~' && (p == word || *(p-1) == '/') && !in_double_quotes) {
            const char *home = getenv("HOME");
            if (!home) home = "";
            
            if (append_str(&buf, &len, &capacity, home) < 0) {
                free(buf);
                return strdup(word);
            }
            p++;
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
                char *s = itoa(getpid());
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

                /* read var name — stop at }, [, # */
                while (*p && *p != '}' && *p != '[') p++;

                size_t var_len = p - var_start;
                char var_name[64] = {0};
                if (var_len > 0 && var_len < 64)
                    strncpy(var_name, var_start, var_len);

                /* ${#arr[@]} or ${#var} — length */
                int get_length = 0;
                if (var_name[0] == '#') {
                    get_length = 1;
                    memmove(var_name, var_name + 1, strlen(var_name));
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
                            /* ${#arr[@]} — element count */
                            char nbuf[16];
                            snprintf(nbuf, sizeof(nbuf), "%d", arr_len(var_name));
                            append_str(&buf, &len, &capacity, nbuf);
                        } else {
                            /* ${#arr[0]} — length of single element */
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
                        (unsigned long)snprintf(nbuf, sizeof(nbuf), "%lu", v ? strlen(v) : 0);
                        append_str(&buf, &len, &capacity, nbuf);
                    } else {
                        const char *var_value = var_get(var_name);
                        if (!var_value) var_value = "";
                        append_str(&buf, &len, &capacity, var_value);
                    }
                    continue;
                }
                /* malformed — treat literally */
                p = var_start - 1;
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
        if (toks[i].type == TOK_WORD && toks[i].value) {
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
