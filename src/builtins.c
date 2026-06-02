//
// Created by mete on 23.04.2026.
//

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fnmatch.h>
#include <regex.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <time.h>
#include <sys/resource.h>
#include <dirent.h>
#include <glob.h>
#include "../include/jobs.h"
#include "../include/alias.h"
#include "../include/rc.h"
#include "../include/shell.h"
#include "../include/signals.h"


extern void  cd_visit(const char *path);
extern char *cd_frecency_top(const char *query);
extern void history_close(void);
extern void alias_free(void);
extern void plugins_unload(void);
extern char *g_trap_actions[TRAP_NSIG];
extern char *g_trap_exit;
extern void  trap_run_exit(int code);
extern void trap_generic_handler(int sig);
extern int g_opt_errexit;
extern int g_opt_xtrace;
extern int g_opt_pipefail;

#define MAX_HASH_ENTRIES 128
typedef struct { char name[64]; char path[256]; int hits; } HashEntry;
static HashEntry g_hash_table[MAX_HASH_ENTRIES];
static int g_hash_count = 0;

static void restore_terminal(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag |= (ICANON | ECHO | ISIG);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    /* move to new line so prompt is clean */
    write(STDOUT_FILENO, "\r\n", 2);
}

static void echo_print_escaped(const char *s) {
    while (*s) {
        if (*s == '\\' && *(s+1)) {
            s++;
            switch (*s) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case '\\': putchar('\\'); break;
                case 'a': putchar('\a'); break;
                case 'b': putchar('\b'); break;
                case '0': putchar('\0'); break;
                default: putchar('\\'); putchar(*s); break;
            }
            s++;
        } else {
            putchar(*s++);
        }
    }
}

int is_builtin(const char *cmd) {
    if (!cmd) return 0;
    if (strcmp(cmd, "break")    == 0) return 1;
    if (strcmp(cmd, "continue") == 0) return 1;
    if (strcmp(cmd, "return") == 0) return 1;
    if (strcmp(cmd, "true")  == 0) return 1;
    if (strcmp(cmd, "false") == 0) return 1;
    if (strcmp(cmd, ":")     == 0) return 1;
    return (strcmp(cmd, "cd") == 0) ||
           (strcmp(cmd, "exit") == 0) ||
           (strcmp(cmd, "exec") == 0) ||
           (strcmp(cmd, "eval") == 0) ||
           (strcmp(cmd, "declare") == 0) ||
           (strcmp(cmd, "typeset") == 0) ||
           (strcmp(cmd, "local")   == 0) ||
           (strcmp(cmd, "readonly") == 0) ||
           (strcmp(cmd, "export") == 0) ||
           (strcmp(cmd, "set") == 0) ||
           (strcmp(cmd, "unset") == 0) ||
           (strcmp(cmd, "pwd") == 0) ||
           (strcmp(cmd, "echo") == 0) ||
           (strcmp(cmd, "jobs") == 0) ||
           (strcmp(cmd, "fg") == 0) ||
           (strcmp(cmd, "bg") == 0) ||
           (strcmp(cmd, "trap") == 0) ||
           (strcmp(cmd, "alias") == 0) ||
           (strcmp(cmd, "unalias") == 0) ||
           (strcmp(cmd, "source") == 0) ||
           (strcmp(cmd, ".") == 0) ||
           (strcmp(cmd, "[[")   == 0) ||
           (strcmp(cmd, "]]")   == 0) ||
           (strcmp(cmd, "((")   == 0) ||
           (strcmp(cmd, "printf") == 0) ||
           (strcmp(cmd, "read") == 0) ||
           (strcmp(cmd, "test") == 0) ||
           (strcmp(cmd, "[")    == 0) ||
           (strcmp(cmd, "getopts")  == 0) ||
           (strcmp(cmd, "mapfile")  == 0) ||
           (strcmp(cmd, "readarray") == 0) ||
           (strcmp(cmd, "type")     == 0) ||
           (strcmp(cmd, "hash")     == 0) ||
           (strcmp(cmd, "wait")     == 0) ||
           (strcmp(cmd, "disown")   == 0) ||
           (strcmp(cmd, "umask")    == 0) ||
           (strcmp(cmd, "ulimit")   == 0) ||
           (strcmp(cmd, "caller")   == 0) ||
           (strcmp(cmd, "compgen")  == 0) ||
           (strcmp(cmd, "complete") == 0) ||
           (strcmp(cmd, "select")   == 0);
}
/* ===== POSIX test / [ implementation ===== */

static int test_file_op(const char *op, const char *path) {
    struct stat st;
    if (strcmp(op, "-e") == 0) return stat(path, &st) == 0;
    if (strcmp(op, "-f") == 0) return stat(path, &st) == 0 && S_ISREG(st.st_mode);
    if (strcmp(op, "-d") == 0) return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    if (strcmp(op, "-r") == 0) return access(path, R_OK) == 0;
    if (strcmp(op, "-w") == 0) return access(path, W_OK) == 0;
    if (strcmp(op, "-x") == 0) return access(path, X_OK) == 0;
    if (strcmp(op, "-s") == 0) return stat(path, &st) == 0 && st.st_size > 0;
    if (strcmp(op, "-L") == 0 ||
        strcmp(op, "-h") == 0) {
        return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
    }
    if (strcmp(op, "-p") == 0) return stat(path, &st) == 0 && S_ISFIFO(st.st_mode);
    if (strcmp(op, "-S") == 0) return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
    if (strcmp(op, "-b") == 0) return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
    if (strcmp(op, "-c") == 0) return stat(path, &st) == 0 && S_ISCHR(st.st_mode);
    if (strcmp(op, "-t") == 0) return isatty(atoi(path));
    if (strcmp(op, "-z") == 0) return 0; /* handled separately */
    if (strcmp(op, "-n") == 0) return 0; /* handled separately */
    return 0;
}

/* forward declaration */
static int builtin_test(char **args, int nargs);

static int test_expr(char **args, int nargs, int *pos);

static int test_primary(char **args, int nargs, int *pos) {
    if (*pos >= nargs) return 0;

    /* "(" expr ")" */
    if (strcmp(args[*pos], "(") == 0) {
        (*pos)++;
        int val = test_expr(args, nargs, pos);
        if (*pos < nargs && strcmp(args[*pos], ")") == 0)
            (*pos)++;
        return val;
    }

    /* "!" primary */
    if (strcmp(args[*pos], "!") == 0) {
        (*pos)++;
        return !test_primary(args, nargs, pos);
    }

    /* unary operators */
    if (*pos + 1 < nargs && args[*pos][0] == '-' && strlen(args[*pos]) == 2) {
        const char *op = args[*pos];
        const char *arg = args[*pos + 1];

        /* string ops */
        if (strcmp(op, "-z") == 0) { *pos += 2; return strlen(arg) == 0; }
        if (strcmp(op, "-n") == 0) { *pos += 2; return strlen(arg) > 0;  }

        /* file ops */
        int r = test_file_op(op, arg);
        *pos += 2;
        return r;
    }

    /* binary operators */
    if (*pos + 2 < nargs) {
        const char *a  = args[*pos];
        const char *op = args[*pos + 1];
        const char *b  = args[*pos + 2];

        /* string comparison */
        if (strcmp(op, "=")  == 0 || strcmp(op, "==") == 0) {
            *pos += 3; return strcmp(a, b) == 0;
        }
        if (strcmp(op, "!=") == 0) { *pos += 3; return strcmp(a, b) != 0; }
        if (strcmp(op, "<")  == 0) { *pos += 3; return strcmp(a, b) < 0;  }
        if (strcmp(op, ">")  == 0) { *pos += 3; return strcmp(a, b) > 0;  }

        /* integer comparison */
        long ia = atol(a), ib = atol(b);
        if (strcmp(op, "-eq") == 0) { *pos += 3; return ia == ib; }
        if (strcmp(op, "-ne") == 0) { *pos += 3; return ia != ib; }
        if (strcmp(op, "-lt") == 0) { *pos += 3; return ia <  ib; }
        if (strcmp(op, "-le") == 0) { *pos += 3; return ia <= ib; }
        if (strcmp(op, "-gt") == 0) { *pos += 3; return ia >  ib; }
        if (strcmp(op, "-ge") == 0) { *pos += 3; return ia >= ib; }

        /* file comparison */
        if (strcmp(op, "-nt") == 0) {
            struct stat sa, sb;
            int ra = stat(a, &sa), rb = stat(b, &sb);
            *pos += 3;
            return (ra == 0 && rb == 0 && sa.st_mtime > sb.st_mtime);
        }
        if (strcmp(op, "-ot") == 0) {
            struct stat sa, sb;
            int ra = stat(a, &sa), rb = stat(b, &sb);
            *pos += 3;
            return (ra == 0 && rb == 0 && sa.st_mtime < sb.st_mtime);
        }
        if (strcmp(op, "-ef") == 0) {
            struct stat sa, sb;
            int ra = stat(a, &sa), rb = stat(b, &sb);
            *pos += 3;
            return (ra == 0 && rb == 0 &&
                    sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino);
        }
    }

    /* single string — true if non-empty */
    const char *s = args[(*pos)++];
    return strlen(s) > 0;
}

static int test_and(char **args, int nargs, int *pos) {
    int val = test_primary(args, nargs, pos);
    while (*pos < nargs && strcmp(args[*pos], "-a") == 0) {
        (*pos)++;
        int right = test_primary(args, nargs, pos);
        val = val && right;
    }
    return val;
}

static int test_expr(char **args, int nargs, int *pos) {
    int val = test_and(args, nargs, pos);
    while (*pos < nargs && strcmp(args[*pos], "-o") == 0) {
        (*pos)++;
        int right = test_and(args, nargs, pos);
        val = val || right;
    }
    return val;
}

static int builtin_test(char **args, int nargs) {
    if (nargs == 0) return 1;  /* false */
    int pos = 0;
    int result = test_expr(args, nargs, &pos);
    return result ? 0 : 1;  /* shell convention: 0=true, 1=false */
}

/* ===== [[ extended test ===== */
static int builtin_double_bracket(char **args, int nargs) {

    if (nargs == 0) return 1;

    for (int i = 0; i < nargs; i++) {
        if (strcmp(args[i], "||") == 0) {
            int left  = builtin_double_bracket(args, i);
            int right = builtin_double_bracket(args + i + 1, nargs - i - 1);
            return (left == 0 || right == 0) ? 0 : 1;
        }
    }
    for (int i = 0; i < nargs; i++) {
        if (strcmp(args[i], "&&") == 0) {
            int left  = builtin_double_bracket(args, i);
            int right = builtin_double_bracket(args + i + 1, nargs - i - 1);
            return (left == 0 && right == 0) ? 0 : 1;
        }
    }

    /* ! */
    if (nargs >= 1 && strcmp(args[0], "!") == 0) {
        return builtin_double_bracket(args + 1, nargs - 1) == 0 ? 1 : 0;
    }

    /* binary ops */
    if (nargs == 3) {
        const char *a  = args[0];
        const char *op = args[1];
        const char *b  = args[2];

        /* glob match */
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            /* fnmatch ile glob */
            return fnmatch(b, a, 0) == 0 ? 0 : 1;
        }
        if (strcmp(op, "!=") == 0) {
            return fnmatch(b, a, 0) != 0 ? 0 : 1;
        }

        /* regex match */
        if (strcmp(op, "=~") == 0) {
            regex_t re;
            if (regcomp(&re, b, REG_EXTENDED) != 0) return 1;
            int r = regexec(&re, a, 0, NULL, 0);
            regfree(&re);
            return r == 0 ? 0 : 1;
        }

        /* string ordering */
        if (strcmp(op, "<") == 0) return strcmp(a, b) < 0 ? 0 : 1;
        if (strcmp(op, ">") == 0) return strcmp(a, b) > 0 ? 0 : 1;

        long ia = atol(a), ib = atol(b);
        if (strcmp(op, "-eq") == 0) return ia == ib ? 0 : 1;
        if (strcmp(op, "-ne") == 0) return ia != ib ? 0 : 1;
        if (strcmp(op, "-lt") == 0) return ia <  ib ? 0 : 1;
        if (strcmp(op, "-le") == 0) return ia <= ib ? 0 : 1;
        if (strcmp(op, "-gt") == 0) return ia >  ib ? 0 : 1;
        if (strcmp(op, "-ge") == 0) return ia >= ib ? 0 : 1;
    }

    int pos = 0;
    int result = test_expr(args, nargs, &pos);
    return result ? 0 : 1;
}

int run_builtin(Command *cmd) {
    if (!cmd || !cmd->argv || cmd->argc == 0) {
        return 1;
    }

    
    const char *builtin_cmd = cmd->argv[0];

    if (strcmp(builtin_cmd, "true") == 0 ||
        strcmp(builtin_cmd, ":")    == 0) {
        return 0;
    }
    if (strcmp(builtin_cmd, "false") == 0) {
        return 1;
    }

    if (strcmp(builtin_cmd, "exec") == 0) {
        /* process replacement */
        if (cmd->argc >= 2) {
            execvp(cmd->argv[1], cmd->argv + 1);
            perror(cmd->argv[1]);
            exit(127);
        }
        /* exec with only redirections: handled by executor's save/restore — nothing to do */
        return 0;
    }

    if (strcmp(cmd->argv[0], "return") == 0) {
        g_return_value = cmd->argc > 1 ? atoi(cmd->argv[1]) : 0;
        g_returning    = 1;
        return g_return_value;
    }
    if (strcmp(builtin_cmd, "eval") == 0) {
        if (cmd->argc < 2) return 0;
        char evalstr[4096] = {0};
        for (int i = 1; i < cmd->argc; i++) {
            if (i > 1) strncat(evalstr, " ", sizeof(evalstr)-strlen(evalstr)-1);
            strncat(evalstr, cmd->argv[i], sizeof(evalstr)-strlen(evalstr)-1);
        }
        extern Token *lex(const char *input, int *ntokens);
        extern Token *glob_expand_tokens(Token *toks, int *ntokens, int last_exit);
        extern Token *word_split_tokens(Token *toks, int ntokens, int *new_count);
        extern CmdList *parse_list(Token *toks, int ntokens);
        extern void tokens_free(Token *toks, int n);
        extern int last_exit_status;
        int ntok = 0;
        Token *toks = lex(evalstr, &ntok);
        if (!toks) return 1;
        toks = glob_expand_tokens(toks, &ntok, last_exit_status);
        if (!toks) return 1;
        toks = word_split_tokens(toks, ntok, &ntok);
        if (!toks) return 1;
        CmdList *cl = parse_list(toks, ntok);
        tokens_free(toks, ntok);
        if (!cl) return 1;
        int r = execute_list(cl);
        extern void cmdlist_free(CmdList *list);
        cmdlist_free(cl);
        return r;
    }
    if (strcmp(builtin_cmd, "set") == 0) {
        if (cmd->argc == 1) {
            /* set with no args: print all variables (basic) */
            extern char **environ;
            for (char **e = environ; *e; e++)
                printf("%s\n", *e);
            return 0;
        }
        for (int i = 1; i < cmd->argc; i++) {
            const char *arg = cmd->argv[i];
            /* set -- args: set positional parameters (check this first!) */
            if (strcmp(arg, "--") == 0) {
                positional_set(cmd->argv + i + 1,
                               cmd->argc - i - 1);
                break;
            }
            /* set -o option */
            if (strcmp(arg, "-o") == 0 && i + 1 < cmd->argc) {
                i++;
                if (strcmp(cmd->argv[i], "pipefail") == 0)
                    g_opt_pipefail = 1;
                else if (strcmp(cmd->argv[i], "errexit") == 0)
                    g_opt_errexit = 1;
                else if (strcmp(cmd->argv[i], "xtrace") == 0)
                    g_opt_xtrace = 1;
                else if (strcmp(cmd->argv[i], "noglob") == 0)
                    ; /* TODO */
                continue;
            }
            /* set +o option */
            if (strcmp(arg, "+o") == 0 && i + 1 < cmd->argc) {
                i++;
                if (strcmp(cmd->argv[i], "pipefail") == 0)
                    g_opt_pipefail = 0;
                else if (strcmp(cmd->argv[i], "errexit") == 0)
                    g_opt_errexit = 0;
                else if (strcmp(cmd->argv[i], "xtrace") == 0)
                    g_opt_xtrace = 0;
                continue;
            }
            /* set -e / -x / -u etc */
            if (arg[0] == '-' || arg[0] == '+') {
                int on = (arg[0] == '-');
                for (int fi = 1; arg[fi]; fi++) {
                    switch (arg[fi]) {
                        case 'e': g_opt_errexit  = on; break;
                        case 'x': g_opt_xtrace   = on; break;
                        case 'u': /* TODO: unset var error */ break;
                        default: break;
                    }
                }
                continue;
            }
        }
        return 0;
    }
    if (strcmp(builtin_cmd, "cd") == 0) {
        const char *target = NULL;

        if (cmd->argc <= 1) {
            /* bare `cd` → $HOME */
            target = getenv("HOME");
            if (!target) { fprintf(stderr, "cd: HOME not set\n"); return 1; }
            if (chdir(target) != 0) { perror("cd"); return 1; }
            cd_visit(target);
            return 0;
        }

        const char *arg = cmd->argv[1];

        /* `cd -` → previous directory */
        if (strcmp(arg, "-") == 0) {
            const char *oldpwd = getenv("OLDPWD");
            if (!oldpwd) { fprintf(stderr, "cd: OLDPWD not set\n"); return 1; }
            char prev[4096];
            strncpy(prev, oldpwd, sizeof(prev)-1);
            char cwd_now[4096];
            if (getcwd(cwd_now, sizeof(cwd_now)))
                setenv("OLDPWD", cwd_now, 1);
            printf("%s\n", prev);
            if (chdir(prev) != 0) { perror("cd"); return 1; }
            setenv("PWD", prev, 1);
            cd_visit(prev);
            return 0;
        }

        /* 1. Exact path — try directly */
        {
            char cwd_before[4096] = {0};
            getcwd(cwd_before, sizeof(cwd_before));

            if (chdir(arg) == 0) {
                char cwd_after[4096];
                if (getcwd(cwd_after, sizeof(cwd_after))) {
                    if (*cwd_before) setenv("OLDPWD", cwd_before, 1);
                    setenv("PWD", cwd_after, 1);
                    cd_visit(cwd_after);
                }
                return 0;
            }
        }

        /* 2. Frecency match — find best scoring dir containing `arg` */
        {
            char *best = cd_frecency_top(arg);
            if (best) {
                struct stat st;
                int valid = (stat(best, &st) == 0 && S_ISDIR(st.st_mode));
                if (valid) {
                    char cwd_before[4096] = {0};
                    getcwd(cwd_before, sizeof(cwd_before));
                    if (chdir(best) == 0) {
                        char cwd_after[4096];
                        if (getcwd(cwd_after, sizeof(cwd_after))) {
                            if (*cwd_before) setenv("OLDPWD", cwd_before, 1);
                            setenv("PWD", cwd_after, 1);
                            cd_visit(cwd_after);
                            fprintf(stderr, "  \033[2;37m→ %s\033[0m\n", cwd_after);
                        }
                        free(best);
                        return 0;
                    }
                }
                free(best);
            }
        }

        /* 3. Nothing matched */
        fprintf(stderr, "cd: %s: no such file or directory\n", arg);
        return 1;
}

    if (strcmp(builtin_cmd, "exit") == 0) {
        int exit_code = (cmd->argc > 1) ? atoi(cmd->argv[1]) : 0;

        /* raw mode → cooked */
        struct termios cooked;
        if (tcgetattr(STDIN_FILENO, &cooked) == 0) {
            cooked.c_lflag |= (ICANON | ECHO | ISIG);
            cooked.c_iflag |= ICRNL;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &cooked);
        }

        tcsetpgrp(STDIN_FILENO, getpgrp());

        history_close();
        alias_free();
        plugins_unload();

        write(STDOUT_FILENO, "\r\n", 2);
        /* restore terminal before running EXIT trap */
        struct termios t;
        if (tcgetattr(STDIN_FILENO, &t) == 0) {
            t.c_lflag |= (ICANON | ECHO | ISIG);
            t.c_iflag |= ICRNL;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
        }
        trap_run_exit(exit_code);
    }

    
    if (strcmp(builtin_cmd, "export") == 0) {
        if (cmd->argc <= 1) {
            return 1;
        }
        
        char *eq = strchr(cmd->argv[1], '=');
        if (!eq) {
            return 1;
        }
        
        *eq = '\0';
        const char *key = cmd->argv[1];
        const char *value = eq + 1;
        
        if (setenv(key, value, 1) != 0) {
            *eq = '='; // Restore the string
            return 1;
        }
        *eq = '='; // Restore the string
        return 0;
    }
    
    if (strcmp(builtin_cmd, "unset") == 0) {
        if (cmd->argc <= 1) {
            return 1;
        }
        var_unset(cmd->argv[1]);
        return 0;
    }
    
    if (strcmp(builtin_cmd, "pwd") == 0) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
            return 0;
        } else {
            perror("pwd");
            return 1;
        }
    }
    
    if (strcmp(builtin_cmd, "echo") == 0) {
        int escape = 0;
        int start_arg = 1;
        if (cmd->argc >= 2 && strcmp(cmd->argv[1], "-e") == 0) {
            escape = 1;
            start_arg = 2;
        }
        int no_newline = 0;
        if (cmd->argc >= 2 && strcmp(cmd->argv[start_arg-1+!escape], "-n") == 0) {
            no_newline = 1;
            start_arg++;
        }
        
        for (int i = start_arg; i < cmd->argc; i++) {
            if (i > start_arg) {
                putchar(' ');
            }
            if (escape) {
                echo_print_escaped(cmd->argv[i]);
            } else {
                fputs(cmd->argv[i], stdout);
            }
        }
        
        if (!no_newline) {
            putchar('\n');
        }
        
        return 0;
    }
    
    if (strcmp(builtin_cmd, "jobs") == 0) {
        jobs_print();
        return 0;
    }
    
    if (strcmp(builtin_cmd, "fg") == 0) {
        int job_id = -1;
        
        if (cmd->argc > 1) {
            // Parse job ID if provided
            if (cmd->argv[1][0] == '%') {
                job_id = atoi(cmd->argv[1] + 1);
            } else {
                fprintf(stderr, "fg: invalid job specification\n");
                return 1;
            }
        }
        
        Job *job;
        if (job_id == -1) {
            // Find the last active job
            job = NULL;
            for (int i = MAX_JOBS - 1; i >= 0; i--) {
                job = job_get_by_id(i + 1); // Assuming IDs start from 1
                if (job) {
                    break;
                }
            }
            if (!job) {
                fprintf(stderr, "fg: no current job\n");
                return 1;
            }
        } else {
            job = job_get_by_id(job_id);
            if (!job) {
                fprintf(stderr, "fg: no such job\n");
                return 1;
            }
        }
        
        pid_t pgid = job->pgid;
        
        // Give the job's process group control of the terminal
        if (tcsetpgrp(STDIN_FILENO, pgid) == -1) {
            perror("tcsetpgrp");
            return 1;
        }
        
        // Send SIGCONT to resume the job if it was stopped
        if (kill(-pgid, SIGCONT) == -1) {
            perror("kill");
            return 1;
        }
        
        // Wait for the job to complete or stop
        int status;
        pid_t result;
        do {
            result = waitpid(-pgid, &status, WUNTRACED);
        } while (result > 0 && !WIFEXITED(status) && !WIFSIGNALED(status) && !WIFSTOPPED(status));
        
        // Take back terminal control
        if (tcsetpgrp(STDIN_FILENO, getpgrp()) == -1) {
            perror("tcsetpgrp");
        }
        
        // Update job status
        if (result > 0) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                job_remove(pgid);
            } else if (WIFSTOPPED(status)) {
                job_set_status(pgid, JOB_STOPPED);
            }
        }
        
        return 0;
    }
    
    if (strcmp(builtin_cmd, "bg") == 0) {
        int job_id = -1;
        pid_t pgid;
        
        if (cmd->argc > 1) {
            // Parse job ID if provided
            if (cmd->argv[1][0] == '%') {
                job_id = atoi(cmd->argv[1] + 1);
            } else {
                job_id = atoi(cmd->argv[1]);
            }
        }
        
        Job *job;
        if (job_id == -1) {
            // Find the last active job
            job = NULL;
            for (int i = MAX_JOBS - 1; i >= 0; i--) {
                job = job_get_by_id(i + 1); // Assuming IDs start from 1
                if (job) {
                    break;
                }
            }
            if (!job) {
                fprintf(stderr, "bg: no current job\n");
                return 1;
            }
        } else {
            job = job_get_by_id(job_id);
            if (!job) {
                fprintf(stderr, "bg: no such job\n");
                return 1;
            }
        }
        
        pgid = job->pgid;
        
        // Send SIGCONT to resume the job
        if (kill(-pgid, SIGCONT) == -1) {
            perror("kill");
            return 1;
        }
        
        // Update job status
        job_set_status(pgid, JOB_RUNNING);
        
        // Print job status
        printf("[%d]+ %s &\n", job->id, job->cmd);
        
        return 0;
    }
    
    if (strcmp(builtin_cmd, "alias") == 0) {
        if (cmd->argc == 1) {
            alias_list();
            return 0;
        }

        char combined[4096] = {0};
        for (int i = 1; i < cmd->argc; i++) {
            if (i > 1) strncat(combined, " ", sizeof(combined)-strlen(combined)-1);
            strncat(combined, cmd->argv[i], sizeof(combined)-strlen(combined)-1);
        }

        char *eq = strchr(combined, '=');
        if (!eq) return 1;

        *eq = '\0';
        char *name = combined;
        char *value = eq + 1;

        while (*value == ' ') value++;

        while (*value == '\'' || *value == '"') {
            char quote = *value;
            int vlen = strlen(value);
            if (vlen >= 2 && value[vlen-1] == quote) {
                value++;
                value[strlen(value)-1] = '\0';
            } else {
                break;
            }
        }

        while ((*value == '\'' || *value == '"')) {
            char quote = *value;
            int vlen = strlen(value);
            if (vlen >= 2 && value[vlen-1] == quote) {
                value++;
                value[strlen(value)-1] = '\0';
            } else {
                break;
            }
        }
        alias_add(name, value);
        return 0;
    }
    if (strcmp(builtin_cmd, "trap") == 0) {
        /* trap with no args: list all active traps */
        if (cmd->argc == 1) {
            for (int i = 0; i < TRAP_NSIG; i++) {
                if (g_trap_actions[i])
                    printf("trap -- '%s' %d\n", g_trap_actions[i], i);
            }
            if (g_trap_exit)
                printf("trap -- '%s' EXIT\n", g_trap_exit);
            return 0;
        }

        /* trap -p [sigspec]: print specific trap */
        if (strcmp(cmd->argv[1], "-p") == 0) {
            if (cmd->argc < 3) {
                /* print all */
                for (int i = 0; i < TRAP_NSIG; i++) {
                    if (g_trap_actions[i])
                        printf("trap -- '%s' %d\n", g_trap_actions[i], i);
                }
                if (g_trap_exit)
                    printf("trap -- '%s' EXIT\n", g_trap_exit);
                return 0;
            }
            const char *spec = cmd->argv[2];
            if (strcasecmp(spec, "EXIT") == 0 || strcmp(spec, "0") == 0) {
                if (g_trap_exit) printf("trap -- '%s' EXIT\n", g_trap_exit);
            } else {
                int sig = atoi(spec);
                if (sig > 0 && sig < TRAP_NSIG && g_trap_actions[sig])
                    printf("trap -- '%s' %d\n", g_trap_actions[sig], sig);
            }
            return 0;
        }

        /* trap '' SIGNAL   — ignore signal
           trap - SIGNAL    — reset to default
           trap ACTION SIG [SIG...] */
        if (cmd->argc < 3) {
            fprintf(stderr, "trap: usage: trap action signal [signal...]\n");
            return 1;
        }

        const char *action = cmd->argv[1];
        char action_buf[4096];
        {
            const char *a = action;
            int alen = strlen(a);
            if (alen >= 2 &&
                ((a[0] == '\'' && a[alen-1] == '\'') ||
                 (a[0] == '"'  && a[alen-1] == '"'))) {
                strncpy(action_buf, a + 1, alen - 2);
                action_buf[alen - 2] = '\0';
                action = action_buf;
                 } else {
                     strncpy(action_buf, a, sizeof(action_buf) - 1);
                     action_buf[sizeof(action_buf)-1] = '\0';
                     action = action_buf;
                 }
        }
        int reset = (strcmp(action, "-") == 0);

        for (int si = 2; si < cmd->argc; si++) {
            const char *spec = cmd->argv[si];

            /* EXIT pseudo-signal */
            if (strcasecmp(spec, "EXIT") == 0 || strcmp(spec, "0") == 0) {
                free(g_trap_exit);
                g_trap_exit = reset ? NULL : strdup(action);
                continue;
            }

            /* map signal name → number */
            int signum = -1;
            /* numeric */
            if (spec[0] >= '0' && spec[0] <= '9') {
                signum = atoi(spec);
            } else {
                /* strip optional SIG prefix */
                const char *name = spec;
                if (strncasecmp(name, "SIG", 3) == 0) name += 3;
                /* common signals */
                struct { const char *n; int s; } map[] = {
                    {"INT",  SIGINT},  {"TERM", SIGTERM}, {"HUP",  SIGHUP},
                    {"QUIT", SIGQUIT}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
                    {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"CHLD", SIGCHLD},
                    {"TSTP", SIGTSTP}, {"CONT", SIGCONT}, {"WINCH",SIGWINCH},
                    {"KILL", SIGKILL}, {"STOP", SIGSTOP}, {NULL, 0}
                };
                for (int mi = 0; map[mi].n; mi++) {
                    if (strcasecmp(name, map[mi].n) == 0) {
                        signum = map[mi].s; break;
                    }
                }
            }

            if (signum < 0 || signum >= TRAP_NSIG) {
                fprintf(stderr, "trap: %s: invalid signal\n", spec);
                continue;
            }

            /* update stored action */
            free(g_trap_actions[signum]);
            g_trap_actions[signum] = reset ? NULL : strdup(action);

            /* install / restore signal handler */
            struct sigaction sa;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_RESTART;

            if (reset) {
                sa.sa_handler = SIG_DFL;
            } else if (action[0] == '\0') {
                sa.sa_handler = SIG_IGN;
            } else {
                sa.sa_handler = trap_generic_handler;
            }
            sigaction(signum, &sa, NULL);
        }
        return 0;
    }
        if (strcmp(builtin_cmd, "printf") == 0) {
        if (cmd->argc < 2) {
            fprintf(stderr, "printf: usage: printf format [args...]\n");
            return 1;
        }
        const char *fmt = cmd->argv[1];
        int argi = 2;
        /* keep looping over format until all args consumed (bash behavior) */
        do {
            for (const char *p = fmt; *p; p++) {
                if (*p != '%' && *p != '\\') { putchar(*p); continue; }
                if (*p == '\\') {
                    p++;
                    switch (*p) {
                        case 'n':  putchar('\n'); break;
                        case 't':  putchar('\t'); break;
                        case 'r':  putchar('\r'); break;
                        case '\\': putchar('\\'); break;
                        case 'a':  putchar('\a'); break;
                        case 'b':  putchar('\b'); break;
                        case 'e':  putchar('\033'); break;
                        case '0': {
                            /* octal: \0NNN */
                            unsigned val = 0;
                            int n = 0;
                            while (n < 3 && *(p+1) >= '0' && *(p+1) <= '7') {
                                p++; val = val * 8 + (*p - '0'); n++;
                            }
                            putchar((char)val);
                            break;
                        }
                        default: putchar('\\'); putchar(*p); break;
                    }
                    continue;
                }
                /* '%' — parse specifier */
                p++;
                /* flags */
                int flag_left = 0, flag_zero = 0;
                while (*p == '-' || *p == '0') {
                    if (*p == '-') flag_left = 1;
                    if (*p == '0') flag_zero = 1;
                    p++;
                }
                /* width */
                int width = 0;
                while (*p >= '0' && *p <= '9') { width = width*10 + (*p-'0'); p++; }
                /* precision */
                int prec = -1;
                if (*p == '.') {
                    p++; prec = 0;
                    while (*p >= '0' && *p <= '9') { prec = prec*10 + (*p-'0'); p++; }
                }
                const char *arg = (argi < cmd->argc) ? cmd->argv[argi++] : "";
                char fmtbuf[64];
                switch (*p) {
                    case 's': {
                        char spec[64];
                        snprintf(spec, sizeof(spec), "%%%s%s%d%s%ds",
                            flag_left ? "-" : "",
                            flag_zero ? "0" : "",
                            width,
                            prec >= 0 ? "." : "",
                            prec >= 0 ? prec : 0);
                        /* avoid format-not-literal warning */
                        if (prec >= 0)
                            printf("%-*.*s", width, prec, arg);
                        else if (flag_left)
                            printf("%-*s", width, arg);
                        else
                            printf("%*s", width, arg);
                        (void)fmtbuf;
                        break;
                    }
                    case 'd': case 'i': {
                        long v = strtol(arg, NULL, 0);
                        if (flag_left)       printf("%-*ld", width, v);
                        else if (flag_zero)  printf("%0*ld", width, v);
                        else                 printf("%*ld",  width, v);
                        break;
                    }
                    case 'u': {
                        unsigned long v = strtoul(arg, NULL, 0);
                        if (flag_left)  printf("%-*lu", width, v);
                        else            printf("%*lu",  width, v);
                        break;
                    }
                    case 'o': {
                        unsigned long v = strtoul(arg, NULL, 0);
                        printf(flag_left ? "%-*lo" : "%*lo", width, v);
                        break;
                    }
                    case 'x': {
                        unsigned long v = strtoul(arg, NULL, 0);
                        printf(flag_left ? "%-*lx" : "%*lx", width, v);
                        break;
                    }
                    case 'X': {
                        unsigned long v = strtoul(arg, NULL, 0);
                        printf(flag_left ? "%-*lX" : "%*lX", width, v);
                        break;
                    }
                    case 'f': case 'e': case 'g': {
                        double v = strtod(arg, NULL);
                        char spec2[32];
                        snprintf(spec2, sizeof(spec2), "%%%s%d%s%d%c",
                            flag_left ? "-" : (flag_zero ? "0" : ""),
                            width, prec >= 0 ? "." : "", prec >= 0 ? prec : 6, *p);
                        printf(spec2, v);
                        break;
                    }
                    case 'c': {
                        int c = arg[0] ? (unsigned char)arg[0] : 0;
                        printf(flag_left ? "%-*c" : "%*c", width ? width : 1, c);
                        break;
                    }
                    case 'b': {
                        /* %b: like %s but interpret backslash escapes */
                        for (const char *q = arg; *q; q++) {
                            if (*q == '\\' && *(q+1)) {
                                q++;
                                switch (*q) {
                                    case 'n':  putchar('\n'); break;
                                    case 't':  putchar('\t'); break;
                                    case 'r':  putchar('\r'); break;
                                    case '\\': putchar('\\'); break;
                                    case 'a':  putchar('\a'); break;
                                    case 'b':  putchar('\b'); break;
                                    case 'e':  putchar('\033'); break;
                                    default:   putchar('\\'); putchar(*q); break;
                                }
                            } else {
                                putchar(*q);
                            }
                        }
                        break;
                    }
                    case '%': putchar('%'); argi--; break;
                    default:  putchar('%'); putchar(*p); break;
                }
            }
        } while (argi < cmd->argc);
        fflush(stdout);
        return 0;
    }

    if (strcmp(builtin_cmd, "read") == 0) {
        /* parse flags */
        int raw = 0, silent = 0;
        const char *prompt = NULL;
        int timeout = -1;
        const char *array_name = NULL;
        int argi = 1;
        int read_fd = -1;  /* -u fd */
        while (argi < cmd->argc && cmd->argv[argi][0] == '-') {
            const char *flag = cmd->argv[argi];
            if (strcmp(flag, "--") == 0) { argi++; break; }
            for (int fi = 1; flag[fi]; fi++) {
                switch (flag[fi]) {
                    case 'r': raw    = 1; break;
                    case 's': silent = 1; break;
                    case 'p':
                        if (flag[fi+1]) { prompt = flag+fi+1; fi = (int)strlen(flag)-1; }
                        else if (argi+1 < cmd->argc) prompt = cmd->argv[++argi];
                        break;
                    case 'u':
                        if (flag[fi+1]) { read_fd = atoi(flag+fi+1); fi = (int)strlen(flag)-1; }
                        else if (argi+1 < cmd->argc) read_fd = atoi(cmd->argv[++argi]);
                        break;
                    case 't':
                        if (flag[fi+1]) { timeout = atoi(flag+fi+1); fi = (int)strlen(flag)-1; }
                        else if (argi+1 < cmd->argc) timeout = atoi(cmd->argv[++argi]);
                        break;
                    case 'a':
                        if (flag[fi+1]) { array_name = flag+fi+1; fi = (int)strlen(flag)-1; }
                        else if (argi+1 < cmd->argc) array_name = cmd->argv[++argi];
                        break;
                    default: break;
                }
            }
            argi++;
        }

        /* variable names to fill */
        char **varnames = cmd->argv + argi;
        int nvarnames   = cmd->argc - argi;

        /* print prompt to stderr (no newline, like bash) */
        /* switch terminal to cooked mode so input works normally */
        struct termios old_term, cooked_term;
        int term_changed = 0;
        if (isatty(STDIN_FILENO)) {
            tcgetattr(STDIN_FILENO, &old_term);
            cooked_term = old_term;
            cooked_term.c_lflag |= (ICANON | ISIG);
            if (silent)
                cooked_term.c_lflag &= ~(tcflag_t)ECHO;
            else
                cooked_term.c_lflag |= ECHO;
            cooked_term.c_iflag |= ICRNL;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &cooked_term);
            tcflush(STDIN_FILENO, TCIFLUSH);
            term_changed = 1;
        }
        int saved_read_fd = -1;
        if (read_fd >= 0) {
            saved_read_fd = dup(STDIN_FILENO);
            dup2(read_fd, STDIN_FILENO);
        }
        /* print prompt directly to stderr after mode switch */
        if (prompt) {
            write(STDERR_FILENO, prompt, strlen(prompt));
        }

        /* setup timeout via select() */
        if (timeout >= 0) {
            fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
            struct timeval tv = { .tv_sec = timeout, .tv_usec = 0 };
            if (select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) <= 0) {
                if (term_changed) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
                return 1;
            }
        }

        /* read one line, respecting raw mode and backslash-continuation */
        char linebuf[4096];
        int  pos = 0;
        int  eof = 0;

        while (1) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n <= 0) { eof = (pos == 0); break; }
            if (c == '\n') break;
            if (!raw && c == '\\') {
                /* backslash-newline: line continuation */
                char c2;
                ssize_t n2 = read(STDIN_FILENO, &c2, 1);
                if (n2 > 0 && c2 == '\n') continue;
                /* not a continuation — store backslash then c2 */
                if (pos < (int)sizeof(linebuf)-1) linebuf[pos++] = '\\';
                if (n2 > 0 && pos < (int)sizeof(linebuf)-1) linebuf[pos++] = c2;
                continue;
            }
            if (pos < (int)sizeof(linebuf)-1) linebuf[pos++] = c;
        }
        linebuf[pos] = '\0';
        

        /* restore to raw mode  */
        if (term_changed) {
            /* move to new line so next prompt starts clean */
            write(STDOUT_FILENO, "\r\n", 2);
            struct termios raw_back;
            tcgetattr(STDIN_FILENO, &raw_back);
            raw_back.c_lflag &= ~(ICANON | ECHO | ISIG);
            raw_back.c_cc[VMIN]  = 1;
            raw_back.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_back);
        }
        if (eof) return 1;

        /* -a: split into array */
        if (array_name) {
            char *tmp = strdup(linebuf);
            char *words[1024];
            int   nw = 0;
            char *tok = strtok(tmp, " \t");
            while (tok && nw < 1024) { words[nw++] = tok; tok = strtok(NULL, " \t"); }
            arr_set_from_list(array_name, words, nw);
            if (saved_read_fd >= 0) {
                dup2(saved_read_fd, STDIN_FILENO);
                close(saved_read_fd);
            }
            free(tmp);
            return 0;
        }

        /* split line into variables by IFS (default: space/tab) */
        const char *ifs = getenv("IFS");
        if (!ifs) ifs = " \t";

        if (nvarnames == 0) {
            /* no variable names given: default to REPLY */
            local_var_set("REPLY", linebuf);
            setenv("REPLY", linebuf, 1);
            if (saved_read_fd >= 0) {
                dup2(saved_read_fd, STDIN_FILENO);
                close(saved_read_fd);
            }
            return 0;
        }

        char *tmp = strdup(linebuf);
        char *p   = tmp;

        for (int vi = 0; vi < nvarnames; vi++) {
            /* skip leading IFS */
            while (*p && strchr(ifs, *p)) p++;
            if (vi == nvarnames - 1) {
                /* last variable gets the remainder (trimmed leading IFS) */

                local_var_set(varnames[vi], p);
                setenv(varnames[vi], p, 1);
                break;
            }
            /* find end of this field */
            char *start = p;
            while (*p && !strchr(ifs, *p)) p++;
            if (*p) *p++ = '\0';
            local_var_set(varnames[vi], start);
            setenv(varnames[vi], start, 1);
        }
        free(tmp);
        if (saved_read_fd >= 0) {
            dup2(saved_read_fd, STDIN_FILENO);
            close(saved_read_fd);
        }
        return 0;
    }

    if (strcmp(builtin_cmd, "source") == 0 ||
        strcmp(builtin_cmd, ".") == 0) {
        if (cmd->argc < 2) {
            fprintf(stderr, "%s: filename required\n", builtin_cmd);
            return 1;
        }
        rc_load(cmd->argv[1]);
        return 0;
    }
    if (strcmp(builtin_cmd, "test") == 0 ||
    strcmp(builtin_cmd, "[")    == 0) {
        int argc = cmd->argc;
        char **argv = cmd->argv;
        int start = 1;

        if (strcmp(builtin_cmd, "[") == 0) {
            if (argc < 2 || strcmp(argv[argc-1], "]") != 0) {
                fprintf(stderr, "[: missing ']'\n");
                return 2;
            }
            argc--;
        }

        int nargs = argc - start;
        char **args = argv + start;

        return builtin_test(args, nargs);
    }

    if (strcmp(builtin_cmd, "local") == 0 ||
        strcmp(builtin_cmd, "readonly") == 0) {
        int attrs = VAR_ATTR_LOCAL;
        if (strcmp(builtin_cmd, "readonly") == 0) attrs |= VAR_ATTR_READONLY;

        for (int i = 1; i < cmd->argc; i++) {
            /* parse optional -r etc. */
            if (cmd->argv[i][0] == '-') {
                for (int fi = 1; cmd->argv[i][fi]; fi++) {
                    if (cmd->argv[i][fi] == 'r') attrs |= VAR_ATTR_READONLY;
                    if (cmd->argv[i][fi] == 'i') attrs |= VAR_ATTR_INTEGER;
                }
                continue;
            }
            char *eq = strchr(cmd->argv[i], '=');
            if (eq) {
                *eq = '\0';
                local_var_set(cmd->argv[i], eq + 1);
                var_declare(cmd->argv[i], eq + 1, attrs);
                *eq = '=';
            } else {
                local_var_set(cmd->argv[i], "");
                var_declare(cmd->argv[i], "", attrs);
            }
        }
        return 0;
    }

    if (strcmp(builtin_cmd, "declare") == 0 ||
        strcmp(builtin_cmd, "typeset") == 0) {
        int attrs = 0;
        int argi  = 1;
        int print = 0;

        while (argi < cmd->argc && cmd->argv[argi][0] == '-') {
            const char *flag = cmd->argv[argi];
            if (strcmp(flag, "--") == 0) { argi++; break; }
            for (int fi = 1; flag[fi]; fi++) {
                switch (flag[fi]) {
                    case 'r': attrs |= VAR_ATTR_READONLY; break;
                    case 'i': attrs |= VAR_ATTR_INTEGER;  break;
                    case 'x': attrs |= VAR_ATTR_EXPORT;   break;
                    case 'n': attrs |= VAR_ATTR_NAMEREF;  break;
                    case 'u': attrs |= VAR_ATTR_UPPERCASE; break;
                    case 'l': attrs |= VAR_ATTR_LOWERCASE; break;
                    case 'p': print = 1; break;
                    default: break;
                }
            }
            argi++;
        }

        if (print || argi >= cmd->argc) {
            var_list(attrs);
            return 0;
        }

        for (int i = argi; i < cmd->argc; i++) {
            char *eq = strchr(cmd->argv[i], '=');
            if (eq) {
                *eq = '\0';
                var_declare(cmd->argv[i], eq + 1, attrs);
                *eq = '=';
            } else {
                /* declare var without value — set attrs only */
                const char *cur = var_get(cmd->argv[i]);
                var_declare(cmd->argv[i], cur, attrs);
            }
        }
        return 0;
    }

    if (strcmp(builtin_cmd, "getopts") == 0) {
        if (cmd->argc < 3) {
            fprintf(stderr, "getopts: usage: getopts optstring name [args...]\n");
            return 1;
        }
        const char *optstring = cmd->argv[1];
        const char *varname   = cmd->argv[2];
        int silent = (optstring[0] == ':');
        const char *opts = silent ? optstring + 1 : optstring;

        /* OPTIND is 1-based index into args */
        const char *oi_str = var_get("OPTIND");
        int optind = oi_str ? atoi(oi_str) : 1;
        if (optind < 1) optind = 1;

        /* within-arg char position (private var _GETOPTS_CP) */
        const char *cp_str = var_get("_GETOPTS_CP");
        int charpos = cp_str ? atoi(cp_str) : 1;
        if (charpos < 1) charpos = 1;

        /* build args array */
        const char *xargs[MAX_ARGS];
        int nxargs;
        if (cmd->argc > 3) {
            nxargs = cmd->argc - 3;
            for (int i = 0; i < nxargs && i < MAX_ARGS; i++)
                xargs[i] = cmd->argv[3 + i];
        } else {
            nxargs = positional_get_count();
            for (int i = 0; i < nxargs && i < MAX_ARGS; i++)
                xargs[i] = positional_get(i);
        }

        /* advance past exhausted args */
        while (optind <= nxargs) {
            const char *a = xargs[optind - 1];
            if (!a || a[0] != '-' || (a[1] == '\0') ||
                (a[1] == '-' && a[2] == '\0')) {
                /* not an option arg */
                char ib[16]; snprintf(ib, sizeof(ib), "%d", optind);
                local_var_set("OPTIND", ib); setenv("OPTIND", ib, 1);
                local_var_set(varname, "?"); setenv(varname, "?", 1);
                return 1;
            }
            if (a[charpos]) break;
            /* exhausted this arg */
            optind++; charpos = 1;
        }

        if (optind > nxargs) {
            char ib[16]; snprintf(ib, sizeof(ib), "%d", optind);
            local_var_set("OPTIND", ib); setenv("OPTIND", ib, 1);
            local_var_set(varname, "?"); setenv(varname, "?", 1);
            return 1;
        }

        const char *curarg = xargs[optind - 1];
        char opt = curarg[charpos];
        const char *found = strchr(opts, opt);

        if (!found) {
            char ob[2] = {opt, 0};
            if (!silent) fprintf(stderr, "zesh: illegal option -- %c\n", opt);
            local_var_set(varname, "?"); setenv(varname, "?", 1);
            local_var_set("OPTARG", ob); setenv("OPTARG", ob, 1);
        } else {
            char ob[2] = {opt, 0};
            local_var_set(varname, ob); setenv(varname, ob, 1);

            if (found[1] == ':') {
                /* option requires argument */
                if (curarg[charpos + 1]) {
                    /* rest of current arg */
                    local_var_set("OPTARG", curarg + charpos + 1);
                    setenv("OPTARG", curarg + charpos + 1, 1);
                    optind++; charpos = 1;
                } else if (optind < nxargs) {
                    optind++;
                    local_var_set("OPTARG", xargs[optind - 1]);
                    setenv("OPTARG", xargs[optind - 1], 1);
                    optind++; charpos = 1;
                } else {
                    local_var_set("OPTARG", ""); setenv("OPTARG", "", 1);
                    if (silent) {
                        char eb[2] = {opt, 0};
                        local_var_set(varname, ":"); setenv(varname, ":", 1);
                        local_var_set("OPTARG", eb); setenv("OPTARG", eb, 1);
                    } else {
                        fprintf(stderr, "zesh: option requires argument -- %c\n", opt);
                        local_var_set(varname, "?"); setenv(varname, "?", 1);
                    }
                    optind++; charpos = 1;
                }
            } else {
                local_var_set("OPTARG", ""); setenv("OPTARG", "", 1);
                charpos++;
                if (!curarg[charpos]) { optind++; charpos = 1; }
            }
        }

        if (found && found[1] != ':') {
            /* charpos already advanced above for non-arg opts */
        }

        char ib2[16]; snprintf(ib2, sizeof(ib2), "%d", optind);
        local_var_set("OPTIND", ib2); setenv("OPTIND", ib2, 1);
        char cb[16]; snprintf(cb, sizeof(cb), "%d", charpos);
        local_var_set("_GETOPTS_CP", cb); setenv("_GETOPTS_CP", cb, 1);
        return found ? 0 : 1;
    }

    if (strcmp(builtin_cmd, "mapfile") == 0 ||
        strcmp(builtin_cmd, "readarray") == 0) {
        const char *array_name = "MAPFILE";
        int count_only = 0;
        int skip = 0;
        int nlines = -1;
        char delim_char = '\n';
        int trim_delim = 0;

        int argi = 1;
        while (argi < cmd->argc && cmd->argv[argi][0] == '-') {
            const char *flag = cmd->argv[argi];
            if (strcmp(flag, "--") == 0) { argi++; break; }
            for (int fi = 1; flag[fi]; fi++) {
                switch (flag[fi]) {
                    case 'n':
                        if (flag[fi+1]) { nlines = atoi(flag+fi+1); fi = (int)strlen(flag)-1; }
                        else if (argi+1 < cmd->argc) nlines = atoi(cmd->argv[++argi]);
                        break;
                    case 's':
                        if (flag[fi+1]) { skip = atoi(flag+fi+1); fi = (int)strlen(flag)-1; }
                        else if (argi+1 < cmd->argc) skip = atoi(cmd->argv[++argi]);
                        break;
                    case 'd':
                        if (flag[fi+1]) { delim_char = flag[fi+1]; fi = (int)strlen(flag)-1; }
                        else if (argi+1 < cmd->argc) delim_char = cmd->argv[++argi][0];
                        break;
                    case 't': /* trim delimiter */ trim_delim = 1; break;
                    default: break;
                }
            }
            argi++;
        }

        if (argi < cmd->argc)
            array_name = cmd->argv[argi];

        /* read lines from stdin */
        char line[4096];
        char **lines = NULL;
        int   nread  = 0;
        int   cap    = 16;
        lines = malloc(cap * sizeof(char*));
        if (!lines) return 1;

        int skipped = 0;
        while (1) {
            if (nlines >= 0 && nread >= nlines) break;
            int li = 0;
            int c;
            while ((c = fgetc(stdin)) != EOF) {
                if (li < (int)sizeof(line) - 1)
                    line[li++] = (char)c;
                if ((char)c == delim_char) break;
            }
            if (li == 0) break;
            /* trim delimiter if -t flag is set */
            if (trim_delim && li > 0 && line[li-1] == delim_char) {
                line[li-1] = '\0';
                li--;
            }
            line[li] = '\0';
            if (skipped < skip) { skipped++; continue; }
            if (nread >= cap) {
                cap *= 2;
                char **tmp = realloc(lines, cap * sizeof(char*));
                if (!tmp) break;
                lines = tmp;
            }
            lines[nread++] = strdup(line);
        }

        /* store in array */
        for (int i = 0; i < nread; i++) {
            arr_set(array_name, i, lines[i]);
            free(lines[i]);
        }
        free(lines);
        (void)count_only;
        return 0;
    }

    if (strcmp(builtin_cmd, "[[") == 0) {
        int nargs = cmd->argc - 1;
        char **args = cmd->argv + 1;
        return builtin_double_bracket(args, nargs);
    }

    if (strcmp(builtin_cmd, "((") == 0) {
        char expr[1024] = {0};
        for (int i = 1; i < cmd->argc; i++) {
            if (i > 1) strncat(expr, " ", sizeof(expr)-strlen(expr)-1);
            strncat(expr, cmd->argv[i], sizeof(expr)-strlen(expr)-1);
        }
        char *res = eval_arithmetic(expr);
        if (!res) return 1;
        long val = atol(res);
        free(res);
        return (val != 0) ? 0 : 1;
    }
    if (strcmp(cmd->argv[0], "break") == 0) {
        g_loop_control = LOOP_BREAK;
        return 0;
    }
    if (strcmp(cmd->argv[0], "continue") == 0) {
        g_loop_control = LOOP_CONTINUE;
        return 0;
    }

    /* ---- type builtin ---- */
    if (strcmp(builtin_cmd, "type") == 0) {
        int all = 0, quiet = 0;
        int argi = 1;
        while (argi < cmd->argc && cmd->argv[argi][0] == '-') {
            for (int fi = 1; cmd->argv[argi][fi]; fi++) {
                if (cmd->argv[argi][fi] == 'a') all = 1;
                if (cmd->argv[argi][fi] == 't') quiet = 1;  /* type only */
                if (cmd->argv[argi][fi] == 'f') { /* force function check */ }
            }
            argi++;
        }
        (void)all;
        int ret = 0;
        for (int i = argi; i < cmd->argc; i++) {
            const char *name = cmd->argv[i];
            /* check keywords */
            const char *keywords[] = {"if","then","else","elif","fi","while",
                "until","do","done","for","in","case","esac","function",
                "select","time","coproc","{","}","!","[[","]]","((",")",NULL};
            int found_kw = 0;
            for (int ki = 0; keywords[ki]; ki++) {
                if (strcmp(name, keywords[ki]) == 0) {
                    if (quiet) printf("keyword\n");
                    else printf("%s is a shell keyword\n", name);
                    found_kw = 1;
                    break;
                }
            }
            if (found_kw) continue;
            /* check function */
            if (func_get_body(name)) {
                if (quiet) printf("function\n");
                else        printf("%s is a function\n", name);
                continue;
            }
            /* check builtin */
            if (is_builtin(name)) {
                if (quiet) printf("builtin\n");
                else        printf("%s is a shell builtin\n", name);
                continue;
            }
            /* check PATH */
            const char *path_env = getenv("PATH");
            int found_ext = 0;
            if (path_env) {
                char pbuf[4096];
                strncpy(pbuf, path_env, sizeof(pbuf)-1);
                char *dir = strtok(pbuf, ":");
                while (dir) {
                    char full[4096];
                    snprintf(full, sizeof(full), "%s/%s", dir, name);
                    if (access(full, X_OK) == 0) {
                        if (quiet) printf("file\n");
                        else        printf("%s is %s\n", name, full);
                        found_ext = 1;
                        break;
                    }
                    dir = strtok(NULL, ":");
                }
            }
            if (!found_ext) {
                fprintf(stderr, "type: %s: not found\n", name);
                ret = 1;
            }
        }
        return ret;
    }

    /* ---- hash builtin ---- */
    if (strcmp(builtin_cmd, "hash") == 0) {
        int do_reset = 0, do_delete = 0;
        const char *del_name = NULL;
        int argi = 1;
        while (argi < cmd->argc && cmd->argv[argi][0] == '-') {
            if (strcmp(cmd->argv[argi], "-r") == 0) {
                do_reset = 1;
                argi++;
            } else if (strcmp(cmd->argv[argi], "-d") == 0) {
                if (argi + 1 < cmd->argc) {
                    do_delete = 1;
                    del_name = cmd->argv[argi + 1];
                    argi += 2;
                } else {
                    argi++;
                }
            } else {
                argi++;
            }
        }
        if (do_reset) {
            g_hash_count = 0;
            memset(g_hash_table, 0, sizeof(g_hash_table));
            return 0;
        }
        if (do_delete && del_name) {
            for (int i = 0; i < g_hash_count; i++) {
                if (strcmp(g_hash_table[i].name, del_name) == 0) {
                    g_hash_table[i] = g_hash_table[--g_hash_count];
                    break;
                }
            }
            return 0;
        }
        if (argi >= cmd->argc) {
            /* list cache */
            if (g_hash_count == 0) return 0;
            printf("hits\tcommand\n");
            for (int i = 0; i < g_hash_count; i++)
                printf("%d\t%s\n", g_hash_table[i].hits, g_hash_table[i].path);
            return 0;
        }
        /* hash NAME — look up and cache */
        for (int ni = argi; ni < cmd->argc; ni++) {
            const char *nm = cmd->argv[ni];
            /* check existing */
            int found = 0;
            for (int i = 0; i < g_hash_count; i++) {
                if (strcmp(g_hash_table[i].name, nm) == 0) {
                    g_hash_table[i].hits++;
                    found = 1; break;
                }
            }
            if (found) continue;
            /* search PATH */
            const char *penv = getenv("PATH");
            if (!penv) { fprintf(stderr, "hash: %s: not found\n", nm); continue; }
            char pbuf[4096]; strncpy(pbuf, penv, sizeof(pbuf)-1);
            char *dir = strtok(pbuf, ":");
            int resolved = 0;
            while (dir) {
                char full[4096];
                snprintf(full, sizeof(full), "%s/%s", dir, nm);
                if (access(full, X_OK) == 0) {
                    if (g_hash_count < MAX_HASH_ENTRIES) {
                        strncpy(g_hash_table[g_hash_count].name, nm, 63);
                        strncpy(g_hash_table[g_hash_count].path, full, 255);
                        g_hash_table[g_hash_count].hits = 1;
                        g_hash_count++;
                    }
                    resolved = 1; break;
                }
                dir = strtok(NULL, ":");
            }
            if (!resolved) fprintf(stderr, "hash: %s: not found\n", nm);
        }
        return 0;
    }

    /* ---- wait builtin ---- */
    if (strcmp(builtin_cmd, "wait") == 0) {
        if (cmd->argc == 1) {
            /* wait for all background jobs */
            int status;
            while (waitpid(-1, &status, 0) > 0);
            return 0;
        }
        int ret2 = 0;
        for (int i = 1; i < cmd->argc; i++) {
            pid_t pid2 = (pid_t)atoi(cmd->argv[i]);
            if (pid2 <= 0) { ret2 = 1; continue; }
            int status2;
            pid_t r = waitpid(pid2, &status2, 0);
            if (r < 0) {
                if (errno == ECHILD) {
                    /* already reaped by SIGCHLD handler — return last status */
                    ret2 = 0;
                } else {
                    fprintf(stderr, "wait: %d: no such process\n", (int)pid2);
                    ret2 = 127;
                }
            } else {
                ret2 = WIFEXITED(status2) ? WEXITSTATUS(status2) : 1;
            }
        }
        return ret2;
    }

    /* ---- disown builtin ---- */
    if (strcmp(builtin_cmd, "disown") == 0) {
        if (cmd->argc == 1) {
            extern void jobs_disown_all(void);
            jobs_disown_all();
            return 0;
        }
        for (int i = 1; i < cmd->argc; i++) {
            if (cmd->argv[i][0] == '%') {
                int jid = atoi(cmd->argv[i] + 1);
                Job *j  = job_get_by_id(jid);
                if (j) job_remove(j->pgid);
            } else {
                pid_t dpid = (pid_t)atoi(cmd->argv[i]);
                job_remove(dpid);
            }
        }
        return 0;
    }

    /* ---- umask builtin ---- */
    if (strcmp(builtin_cmd, "umask") == 0) {
        if (cmd->argc == 1) {
            mode_t m = umask(0); umask(m);
            printf("%04o\n", (unsigned)m);
            return 0;
        }
        mode_t new_mask = (mode_t)strtol(cmd->argv[1], NULL, 8);
        umask(new_mask);
        return 0;
    }

    /* ---- ulimit builtin ---- */
    if (strcmp(builtin_cmd, "ulimit") == 0) {
        struct rlimit rl;
        int resource = RLIMIT_NOFILE;
        int hard = 0, soft = 0;
        int argi2 = 1;

        while (argi2 < cmd->argc && cmd->argv[argi2][0] == '-') {
            for (int fi = 1; cmd->argv[argi2][fi]; fi++) {
                switch(cmd->argv[argi2][fi]) {
                    case 'n': resource = RLIMIT_NOFILE;  break;
                    case 'c': resource = RLIMIT_CORE;    break;
                    case 'f': resource = RLIMIT_FSIZE;   break;
                    case 's': resource = RLIMIT_STACK;   break;
                    case 'v': resource = RLIMIT_AS;      break;
                    case 'H': hard = 1;  break;
                    case 'S': soft = 1;  break;
                    case 'a': {
                        /* print all */
                        int resources[] = {RLIMIT_NOFILE, RLIMIT_CORE, RLIMIT_FSIZE,
                                           RLIMIT_STACK, RLIMIT_AS};
                        const char *names[] = {"open files", "core file size",
                                               "file size", "stack size", "virtual memory"};
                        for (int ri = 0; ri < 5; ri++) {
                            getrlimit(resources[ri], &rl);
                            if (rl.rlim_cur == RLIM_INFINITY)
                                printf("%-24s unlimited\n", names[ri]);
                            else
                                printf("%-24s %lu\n", names[ri], (unsigned long)rl.rlim_cur);
                        }
                        return 0;
                    }
                    default: break;
                }
            }
            argi2++;
        }

        if (argi2 < cmd->argc) {
            /* set limit */
            getrlimit(resource, &rl);
            rlim_t val;
            if (strcmp(cmd->argv[argi2], "unlimited") == 0)
                val = RLIM_INFINITY;
            else
                val = (rlim_t)strtoull(cmd->argv[argi2], NULL, 10);
            if (hard) rl.rlim_max = val;
            if (soft || !hard) rl.rlim_cur = val;
            setrlimit(resource, &rl);
        } else {
            /* get limit */
            getrlimit(resource, &rl);
            rlim_t val2 = (hard && !soft) ? rl.rlim_max : rl.rlim_cur;
            if (val2 == RLIM_INFINITY) printf("unlimited\n");
            else printf("%lu\n", (unsigned long)val2);
        }
        return 0;
    }

    /* ---- caller builtin ---- */
    if (strcmp(builtin_cmd, "caller") == 0) {
        /* caller [expr] — print call stack info */
        /* basic: print line number (0) and function name */
        int level = cmd->argc > 1 ? atoi(cmd->argv[1]) : 0;
        (void)level;
        printf("0 %s\n", g_current_funcname[0] ? g_current_funcname : "main");
        return 0;
    }

    /* ---- compgen/complete ---- */
    if (strcmp(builtin_cmd, "compgen") == 0) {
        int argi3 = 1;
        const char *word_filter = NULL;
        while (argi3 < cmd->argc && cmd->argv[argi3][0] == '-') {
            const char *flag3 = cmd->argv[argi3];
            argi3++;
            if (strcmp(flag3, "-W") == 0 && argi3 < cmd->argc) {
                /* -W wordlist: print matching words */
                const char *wordlist = cmd->argv[argi3++];
                if (argi3 < cmd->argc) word_filter = cmd->argv[argi3++];
                /* split wordlist on spaces and print matches */
                char wl[4096]; strncpy(wl, wordlist, sizeof(wl)-1);
                char *tok3 = strtok(wl, " \t");
                while (tok3) {
                    if (!word_filter || strncmp(tok3, word_filter,
                                                strlen(word_filter)) == 0)
                        printf("%s\n", tok3);
                    tok3 = strtok(NULL, " \t");
                }
                return 0;
            }
            if (strcmp(flag3, "-b") == 0) {
                /* generate builtin names */
                if (argi3 < cmd->argc) word_filter = cmd->argv[argi3];
                const char *builtins[] = {
                    "cd","echo","exit","export","unset","pwd","jobs","fg","bg",
                    "alias","unalias","source","trap","printf","read","test",
                    "local","declare","typeset","readonly","set","getopts",
                    "mapfile","readarray","type","hash","wait","disown","umask",
                    "ulimit","caller","compgen","complete","return","break",
                    "continue","true","false",":",NULL
                };
                for (int bi = 0; builtins[bi]; bi++) {
                    if (!word_filter || strncmp(builtins[bi], word_filter,
                                                strlen(word_filter)) == 0)
                        printf("%s\n", builtins[bi]);
                }
                return 0;
            }
            if (strcmp(flag3, "-k") == 0) {
                /* generate keywords */
                if (argi3 < cmd->argc) word_filter = cmd->argv[argi3];
                const char *keywords[] = {
                    "if","then","else","elif","fi","while","until","do","done",
                    "for","in","case","esac","function","select","time","coproc",
                    "[[","]]","((",")){","}","!",NULL
                };
                for (int ki = 0; keywords[ki]; ki++) {
                    if (!word_filter || strncmp(keywords[ki], word_filter,
                                                strlen(word_filter)) == 0)
                        printf("%s\n", keywords[ki]);
                }
                return 0;
            }
            if (strcmp(flag3, "-c") == 0) {
                /* generate commands */
                if (argi3 < cmd->argc) word_filter = cmd->argv[argi3];
                const char *penv = getenv("PATH");
                if (penv) {
                    char pbuf3[4096]; strncpy(pbuf3, penv, sizeof(pbuf3)-1);
                    char *dir3 = strtok(pbuf3, ":");
                    while (dir3) {
                        DIR *dp = opendir(dir3);
                        if (dp) {
                            struct dirent *ent;
                            while ((ent = readdir(dp))) {
                                if (ent->d_name[0] == '.') continue;
                                if (!word_filter || strncmp(ent->d_name, word_filter, strlen(word_filter)) == 0) {
                                    char full3[4096];
                                    snprintf(full3, sizeof(full3), "%s/%s", dir3, ent->d_name);
                                    if (access(full3, X_OK) == 0)
                                        printf("%s\n", ent->d_name);
                                }
                            }
                            closedir(dp);
                        }
                        dir3 = strtok(NULL, ":");
                    }
                }
                return 0;
            }
            if (strcmp(flag3, "-f") == 0) {
                /* generate filenames */
                if (argi3 < cmd->argc) word_filter = cmd->argv[argi3];
                glob_t g3;
                char pat3[256];
                snprintf(pat3, sizeof(pat3), "%s*", word_filter ? word_filter : "");
                if (glob(pat3, GLOB_NOCHECK, NULL, &g3) == 0) {
                    for (size_t gi = 0; gi < g3.gl_pathc; gi++)
                        printf("%s\n", g3.gl_pathv[gi]);
                    globfree(&g3);
                }
                return 0;
            }
            if (strcmp(flag3, "-v") == 0) {
                /* generate variable names */
                extern char **environ;
                if (argi3 < cmd->argc) word_filter = cmd->argv[argi3];
                for (char **ep = environ; *ep; ep++) {
                    char *eq3 = strchr(*ep, '=');
                    size_t nlen = eq3 ? (size_t)(eq3 - *ep) : strlen(*ep);
                    if (!word_filter || strncmp(*ep, word_filter, strlen(word_filter)) == 0)
                        printf("%.*s\n", (int)nlen, *ep);
                }
                return 0;
            }
        }
        return 0;
    }

    if (strcmp(builtin_cmd, "complete") == 0) {
        /* stub: just acknowledge */
        return 0;
    }

    return 1; // Not a builtin command
}
