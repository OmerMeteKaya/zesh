//
// Created by mete on 23.04.2026.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>
#include <fnmatch.h>
#include <regex.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <signal.h>
#include "../include/jobs.h"
#include "../include/alias.h"
#include "../include/rc.h"
#include "../include/shell.h"

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
    return (strcmp(cmd, "cd") == 0) || 
           (strcmp(cmd, "exit") == 0) || 
           (strcmp(cmd, "export") == 0) || 
           (strcmp(cmd, "unset") == 0) || 
           (strcmp(cmd, "pwd") == 0) || 
           (strcmp(cmd, "echo") == 0) ||
           (strcmp(cmd, "jobs") == 0) ||
           (strcmp(cmd, "fg") == 0) ||
           (strcmp(cmd, "bg") == 0) ||
           (strcmp(cmd, "alias") == 0) ||
           (strcmp(cmd, "unalias") == 0) ||
           (strcmp(cmd, "source") == 0) ||
           (strcmp(cmd, ".") == 0) ||
           (strcmp(cmd, "[[")   == 0) ||
           (strcmp(cmd, "]]")   == 0) ||
           (strcmp(cmd, "((")   == 0) ||
           (strcmp(cmd, "test") == 0) ||
           (strcmp(cmd, "[")    == 0);

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
    if (strcmp(cmd->argv[0], "return") == 0) {
        g_return_value = cmd->argc > 1 ? atoi(cmd->argv[1]) : 0;
        g_returning    = 1;
        return g_return_value;
    }
    if (strcmp(builtin_cmd, "cd") == 0) {
        const char *path = getenv("HOME");
        if (cmd->argc > 1) {
            path = cmd->argv[1];
        }
        
        if (chdir(path) != 0) {
            perror("cd");
            return 1;
        }
        return 0;
    }
    
    if (strcmp(builtin_cmd, "exit") == 0) {
        restore_terminal();
        exit(cmd->argc > 1 ? atoi(cmd->argv[1]) : 0);
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
        
        if (unsetenv(cmd->argv[1]) != 0) {
            return 1;
        }
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
    
    return 1; // Not a builtin command
}
