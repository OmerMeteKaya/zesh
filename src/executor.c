//
// Created by mete on 23.04.2026.
//

#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <fnmatch.h>
#include <errno.h>
#include <unistd.h>
#include "../include/jobs.h"
#include "../include/shell.h"

int g_return_value = 0;
int g_returning    = 0;
extern void set_fg_pid(pid_t pid);
extern int  last_exit_status;
volatile int g_interrupt_loop = 0;
LoopControl g_loop_control = LOOP_NORMAL;
static int g_in_procsubst = 0;

static int execute_if(IfNode *node);
static int execute_while(WhileNode *node);
static int execute_for(ForNode *node);
static int execute_list_expanded(CmdList *list);
static int execute_case(CaseNode *node);
static int execute_pipeline_expanded(Pipeline *p);

static int execute_pipeline_expanded(Pipeline *p) {
    if (!p) return 1;

    if (p->ncommands == 1 &&
        p->commands[0].argc == 1 &&
        p->commands[0].argv &&
        p->commands[0].argv[0]) {
        char *arg = p->commands[0].argv[0];
        char *eq  = strchr(arg, '=');
        if (eq && eq != arg && strchr(arg, '[') == NULL) {
            char name[64] = {0};
            strncpy(name, arg, eq - arg);
            char *expanded = expand_word(eq + 1, last_exit_status);
            local_var_set(name, expanded ? expanded : eq + 1);
            if (expanded) free(expanded);
            last_exit_status = 0;
            return 0;
        }
    }
    Pipeline tmp;
    tmp.background = p->background;
    tmp.ncommands  = p->ncommands;
    tmp.commands   = calloc(p->ncommands, sizeof(Command));
    if (!tmp.commands) return 1;

    for (int ci = 0; ci < p->ncommands; ci++) {
        Command *src = &p->commands[ci];
        Command *dst = &tmp.commands[ci];

        dst->argc            = src->argc;
        dst->append          = src->append;
        dst->heredoc_expand  = src->heredoc_expand;
        dst->heredoc_content = src->heredoc_content; /* shared */
        dst->heredoc_delim   = src->heredoc_delim;   /* shared */

        dst->argv = calloc(src->argc + 1, sizeof(char *));
        if (!dst->argv) {
            for (int k = 0; k < ci; k++) {
                for (int ai = 0; ai < tmp.commands[k].argc; ai++)
                    free(tmp.commands[k].argv[ai]);
                free(tmp.commands[k].argv);
                free(tmp.commands[k].infile);
                free(tmp.commands[k].outfile);
            }
            free(tmp.commands);
            return 1;
        }
        for (int ai = 0; ai < src->argc; ai++) {
            if (!src->argv[ai]) { dst->argv[ai] = NULL; continue; }
            char *ex = expand_word(src->argv[ai], last_exit_status);
            dst->argv[ai] = ex ? ex : strdup(src->argv[ai]);
        }
        dst->argv[src->argc] = NULL;

        dst->infile  = src->infile  ? expand_word(src->infile,  last_exit_status) : NULL;
        dst->outfile = src->outfile ? expand_word(src->outfile, last_exit_status) : NULL;
    }

    #define TMP_FREE() do { \
        for (int _ci = 0; _ci < tmp.ncommands; _ci++) { \
            if (tmp.commands[_ci].argv) { \
                for (int _ai = 0; _ai < tmp.commands[_ci].argc; _ai++) \
                    free(tmp.commands[_ci].argv[_ai]); \
                free(tmp.commands[_ci].argv); \
            } \
            free(tmp.commands[_ci].infile); \
            free(tmp.commands[_ci].outfile); \
        } \
        free(tmp.commands); \
    } while(0)

    int status = 0;

    if (tmp.ncommands == 0 || tmp.commands[0].argc == 0 ||
        !tmp.commands[0].argv || !tmp.commands[0].argv[0]) {
        TMP_FREE();
        return 0;
    }

    CmdList *fbody = func_get_body(tmp.commands[0].argv[0]);
    if (fbody) {
        positional_set(tmp.commands[0].argv + 1,
                       tmp.commands[0].argc - 1);
        status = execute_list_expanded(fbody);
        positional_clear();
        g_returning = 0;
        TMP_FREE();
        return status;
    }

    /* builtin */
    if (is_builtin(tmp.commands[0].argv[0])) {
        Command *bcmd    = &tmp.commands[0];
        int saved_stdout = -1, saved_stdin = -1;

        if (bcmd->outfile) {
            int flags = O_WRONLY | O_CREAT |
                        (bcmd->append ? O_APPEND : O_TRUNC);
            int fd = open(bcmd->outfile, flags, 0644);
            if (fd >= 0) {
                saved_stdout = dup(STDOUT_FILENO);
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }
        if (bcmd->infile) {
            int fd = open(bcmd->infile, O_RDONLY);
            if (fd >= 0) {
                saved_stdin = dup(STDIN_FILENO);
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
        }

        status = run_builtin(bcmd);

        if (saved_stdout >= 0) {
            fflush(stdout);
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        if (saved_stdin >= 0) {
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
        }
        TMP_FREE();
        return status;
    }

    /* external command */
    status = execute(&tmp);
    TMP_FREE();
    return status;

    #undef TMP_FREE
}
static int execute_list_expanded(CmdList *list) {
    if (!list || list->count == 0) return 0;

    int last_status = 0;

    for (int i = 0; i < list->count; i++) {
        CmdNode *node = &list->nodes[i];

        if (i > 0) {
            ListOp prev_op = list->nodes[i - 1].op;
            if (prev_op == OP_AND && last_status != 0) continue;
            if (prev_op == OP_OR  && last_status == 0) continue;
        }
        if (g_sigint_received || g_interrupt_loop) {
            return 130;
        }


        switch (node->type) {
            case NODE_IF:
                last_status = execute_if(node->if_node);
                break;
            case NODE_WHILE:
                last_status = execute_while(node->while_node);
                break;
            case NODE_CASE:
                last_status = execute_case(node->case_node);
                break;
            case NODE_FOR:
                last_status = execute_for(node->for_node);
                break;
            case NODE_PIPELINE:
            default:
                if (node->pipeline)
                    last_status = execute_pipeline_expanded(node->pipeline);
                break;
        }

        last_exit_status = last_status;
        if (g_loop_control != LOOP_NORMAL) return last_status;
        if (g_returning) return g_return_value;
    }

    return last_status;
}
/* ------------------------------------------------------------------ */
/*  execute_if                                                          */
/* ------------------------------------------------------------------ */

static int execute_if(IfNode *node) {
    if (!node) return 1;

    /* evaluate condition */
    int cond = execute_list_expanded(node->condition);

    if (cond == 0) {
        /* condition true → run then_body */
        if (node->then_body)
            return execute_list_expanded(node->then_body);
        return 0;
    }

    /* elif chain */
    for (int i = 0; i < node->elif_count; i++) {
        int elif_cond = execute_list_expanded(node->elif_conditions[i]);
        if (elif_cond == 0) {
            if (node->elif_bodies[i])
                return execute_list_expanded(node->elif_bodies[i]);
            return 0;
        }
    }

    /* else */
    if (node->else_body)
        return execute_list_expanded(node->else_body);

    return 1; /* no branch taken — exit status 1 like bash */
}

/* ------------------------------------------------------------------ */
/*  execute_while                                                       */
/* ------------------------------------------------------------------ */

static int execute_while(WhileNode *node) {
    if (!node) return 1;
    int status = 0;

    while (1) {
        if (g_sigint_received || g_interrupt_loop) {
            g_sigint_received = 0;
            g_interrupt_loop  = 0;
            return 130;
        }

        int cond = execute_list_expanded(node->condition);

        if (g_sigint_received || g_interrupt_loop) {
            g_sigint_received = 0;
            g_interrupt_loop  = 0;
            return 130;
        }

        if (node->is_until ? (cond == 0) : (cond != 0)) break;

        if (node->body)
            status = execute_list_expanded(node->body);

        if (g_sigint_received || g_interrupt_loop) {
            g_sigint_received = 0;
            g_interrupt_loop  = 0;
            return 130;
        }

        if (g_loop_control == LOOP_BREAK) {
            g_loop_control = LOOP_NORMAL; break;
        }
        if (g_loop_control == LOOP_CONTINUE) {
            g_loop_control = LOOP_NORMAL; continue;
        }
    }
    return status;
}

static int execute_case(CaseNode *node) {

    if (!node) return 1;

    char *word = expand_word(node->word, last_exit_status);
    if (!word) word = strdup(node->word);

    int status = 0;
    for (int i = 0; i < node->nitem; i++) {
        char *pattern = expand_word(node->items[i].pattern,
                                     last_exit_status);
        if (!pattern) pattern = strdup(node->items[i].pattern);

        /* glob match */
        int match = (fnmatch(pattern, word, 0) == 0);

        if (strcmp(pattern, "*") == 0) match = 1;

        free(pattern);

        if (match) {
            if (node->items[i].body)
                status = execute_list_expanded(node->items[i].body);
            free(word);
            return status;
        }
    }

    free(word);
    return 0;
}
/* ------------------------------------------------------------------ */
/*  execute_for                                                         */
/* ------------------------------------------------------------------ */

static int execute_for(ForNode *node) {
    if (!node) return 1;
    int status = 0;
    if (node->nwords == 0) return 0;

    for (int i = 0; i < node->nwords; i++) {
        if (g_sigint_received || g_interrupt_loop) {
            g_sigint_received = 0;
            g_interrupt_loop  = 0;
            return 130;
        }

        char *expanded = expand_word(node->words[i], last_exit_status);
        const char *val = expanded ? expanded : node->words[i];
        local_var_set(node->var, val);
        if (expanded) free(expanded);

        if (node->body)
            status = execute_list_expanded(node->body);

        if (g_sigint_received || g_interrupt_loop) {
            g_sigint_received = 0;
            g_interrupt_loop  = 0;
            return 130;
        }

        if (g_loop_control == LOOP_BREAK) {
            g_loop_control = LOOP_NORMAL; break;
        }
        if (g_loop_control == LOOP_CONTINUE) {
            g_loop_control = LOOP_NORMAL; continue;
        }
    }
    return status;
}



int execute_list_in_subshell(CmdList *list) {
    /*
     * Like execute_list but runs builtins in a forked child.
     * Used by process substitution children where stdout/stdin
     * are already redirected to a pipe — we must not dup/restore fds.
     */
    if (!list || list->count == 0) return 0;
    g_in_procsubst = 1;
    int last_status = 0;

    for (int i = 0; i < list->count; i++) {
        CmdNode *node = &list->nodes[i];
        if (!node->pipeline) continue;

        if (i > 0) {
            ListOp prev_op = list->nodes[i-1].op;
            if (prev_op == OP_AND && last_status != 0) continue;
            if (prev_op == OP_OR  && last_status == 0) continue;
        }

        /* Always go through execute() — never the in-process builtin path */
        last_status = execute(node->pipeline);
        last_exit_status = last_status;
    }
    g_in_procsubst = 0;
    return last_status;
}

/* ------------------------------------------------------------------ */
/*                         execute_list                               */
/* ------------------------------------------------------------------ */

int execute_list(CmdList *list) {
    if (!list || list->count == 0) return 0;

    int last_status = 0;

    for (int i = 0; i < list->count; i++) {
        CmdNode *node = &list->nodes[i];

        /* --- && / || short-circuit --- */
        if (i > 0) {
            ListOp prev_op = list->nodes[i - 1].op;
            if (prev_op == OP_AND && last_status != 0) continue;
            if (prev_op == OP_OR  && last_status == 0) continue;
        }
        if (g_sigint_received || g_interrupt_loop) {
            return 130;
        }

        /* --- dispatch on node type --- */
        switch (node->type) {
            case NODE_FUNC:
                last_status = 0;
                break;
        case NODE_IF:
            last_status = execute_if(node->if_node);
            break;

        case NODE_WHILE:
            last_status = execute_while(node->while_node);
            break;
        case NODE_CASE:
            last_status = execute_case(node->case_node);
             break;

        case NODE_FOR:
            last_status = execute_for(node->for_node);
            break;

case NODE_PIPELINE:
default: {
    Pipeline *p = node->pipeline;
    if (!p) { last_status = 0; break; }

    if (p->ncommands == 0 || !p->commands[0].argv ||
        !p->commands[0].argv[0]) { last_status = 0; break; }

    /* inline assignment */
    if (p->ncommands == 1 && p->commands[0].argc == 1) {
        char *arg = p->commands[0].argv[0];
        char *eq  = strchr(arg, '=');
        if (eq && eq != arg && strchr(arg, '[') == NULL) {
            char name[64] = {0};
            strncpy(name, arg, eq - arg);
            char *expanded = expand_word(eq + 1, last_exit_status);
            local_var_set(name, expanded ? expanded : eq + 1);
            if (expanded) free(expanded);
            last_status = 0;
            last_exit_status = 0;
            break;
        }
    }

    CmdList *fbody = func_get_body(p->commands[0].argv[0]);
    if (fbody) {
        positional_set(p->commands[0].argv + 1,
                       p->commands[0].argc - 1);
        last_status = execute_list_expanded(fbody);
        positional_clear();
        g_returning = 0;
        last_exit_status = last_status;
        break;
    }

    /* builtin */
    if (is_builtin(p->commands[0].argv[0])) {
        Command *bcmd    = &p->commands[0];
        int saved_stdout = -1, saved_stdin = -1;

        if (bcmd->outfile) {
            int flags = O_WRONLY | O_CREAT |
                        (bcmd->append ? O_APPEND : O_TRUNC);
            int fd = open(bcmd->outfile, flags, 0644);
            if (fd >= 0) {
                saved_stdout = dup(STDOUT_FILENO);
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }
        if (bcmd->infile) {
            int fd = open(bcmd->infile, O_RDONLY);
            if (fd >= 0) {
                saved_stdin = dup(STDIN_FILENO);
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
        }

        last_status = run_builtin(bcmd);

        if (saved_stdout >= 0) {
            fflush(stdout);
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        if (saved_stdin >= 0) {
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
        }
        break;
    }

    /* external */
    last_status = execute(p);
    break;
}
        } /* switch */

        last_exit_status = last_status;
        if (g_loop_control != LOOP_NORMAL) return last_status;
        if (g_returning) return g_return_value;
    }

    ps_fds_close();
    ps_pids_wait();

    return last_status;
}
int execute(Pipeline *p) {
    if (!p || p->ncommands == 0) {
        return -1;
    }

    // Special case: no commands (should not happen if parser is correct)
    if (p->ncommands == 0) {
        return 0;
    }

    // Single command case
    if (p->ncommands == 1) {
        Command *cmd = &p->commands[0];
        
        /* --- PARENT: setup heredoc pipe before fork --- */
        int heredoc_pipe[2] = {-1, -1};
        if (cmd->heredoc_content) {
            if (pipe(heredoc_pipe) < 0) {
                perror("pipe");
                return -1;
            }
            
            /* expand variables in heredoc content */
            const char *content_to_write = cmd->heredoc_content;
            char *expanded_content = NULL;
            if (cmd->heredoc_expand) {
                extern char *expand_word(const char *word, int last_exit);
                extern int last_exit_status;
                
                expanded_content = expand_word(cmd->heredoc_content,
                                                last_exit_status);
                if (expanded_content) content_to_write = expanded_content;
            }
            
            size_t hlen = strlen(content_to_write);
            size_t written = 0;
            while (written < hlen) {
                ssize_t w = write(heredoc_pipe[1],
                                  content_to_write + written,
                                  hlen - written);
                if (w <= 0) break;
                written += w;
            }
            close(heredoc_pipe[1]);
            free(expanded_content);
        }

        pid_t pid = fork();
        if (pid == 0) {
            signals_child();

            /* heredoc stdin */
            if (heredoc_pipe[0] >= 0) {
                dup2(heredoc_pipe[0], STDIN_FILENO);
                close(heredoc_pipe[0]);
            } else if (cmd->infile) {
                int fd = open(cmd->infile, O_RDONLY);
                if (fd < 0) {
                    perror(cmd->infile);
                    _exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            /* outfile handling — existing code */
            if (cmd->outfile) {
                int flags = O_WRONLY|O_CREAT|(cmd->append?O_APPEND:O_TRUNC);
                int fd = open(cmd->outfile, flags, 0644);
                if (fd < 0) {
                    perror(cmd->outfile);
                    _exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            // Execute the command
            execvp(cmd->argv[0], cmd->argv);
            perror(cmd->argv[0]);
            _exit(127);
        }  else if (pid > 0) {
        if (heredoc_pipe[0] >= 0) close(heredoc_pipe[0]);

        setpgid(pid, pid);

        /* ── background ── */
        if (p->background) {
            char cmd_str[256] = {0};
            for (int i = 0; i < cmd->argc && i < 10; i++) {
                if (i > 0) strncat(cmd_str, " ",
                                   sizeof(cmd_str)-strlen(cmd_str)-1);
                strncat(cmd_str, cmd->argv[i],
                        sizeof(cmd_str)-strlen(cmd_str)-1);
            }
            int job_id = job_add(pid, cmd_str);
            printf("[%d] %d\n", job_id, pid);
            return 0;
        }

        /* ── foreground ── */

            if (!g_in_procsubst) tcsetpgrp(STDIN_FILENO, pid);
            if (!g_in_procsubst) set_fg_pid(pid);



            int status = 0;
            pid_t result;
            int wait_count = 0;


            do {
                result = waitpid(pid, &status, WUNTRACED);
                wait_count++;

            } while (result == -1 && errno == EINTR
                     && !g_sigint_received
                     && !g_interrupt_loop);


            set_fg_pid(0);
            if (!g_in_procsubst) tcsetpgrp(STDIN_FILENO, getpgrp());



        /* ── SIGINT / interrupt ── */
            if (g_sigint_received || g_interrupt_loop) {
                if (result == -1 && pid > 0) {
                    kill(pid, SIGINT);
                    waitpid(pid, &status, 0);
                } else if (result > 0 && WIFSTOPPED(status)) {
                    kill(pid, SIGINT);
                    kill(pid, SIGCONT);
                    waitpid(pid, &status, 0);
                }
                write(STDOUT_FILENO, "\r\n", 2);
                return 130;
            }

            /* Ctrl+Z: stopped */
            if (result > 0 && WIFSTOPPED(status)) {
                char cmd_str[256] = {0};
                for (int i = 0; i < cmd->argc && i < 10; i++) {
                    if (i > 0) strncat(cmd_str, " ",
                                       sizeof(cmd_str)-strlen(cmd_str)-1);
                    strncat(cmd_str, cmd->argv[i],
                            sizeof(cmd_str)-strlen(cmd_str)-1);
                }
                int job_id = job_add(pid, cmd_str);
                job_set_status(pid, JOB_STOPPED);
                write(STDOUT_FILENO, "\r\n", 2);
                printf("[%d]+  Stopped\t\t%s\n", job_id, cmd_str);
                /*
                 * Loop içinde Ctrl+Z — döngüyü durdur.
                 * g_loop_control = LOOP_BREAK ile execute_while/for
                 * bir sonraki iterasyonda durur.
                 */
                g_loop_control = LOOP_BREAK;
                return 0;
            }
            if (WIFEXITED(status))   return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                if (sig == SIGINT) {
                    g_sigint_received = 1;
                    write(STDOUT_FILENO, "\r\n", 2);
                }
                return 128 + sig;
            }
            return -1;



    } else {
        /* fork failed */
        perror("fork");
        return -1;
    }
    } else {
        // Multiple commands with pipes
        int **pipes = malloc((p->ncommands - 1) * sizeof(int*));
        if (!pipes) {
            perror("malloc");
            return -1;
        }
        
        /* setup heredoc pipes for each command */
        int heredoc_pipes[MAX_ARGS][2];
        for (int i = 0; i < p->ncommands; i++) {
            heredoc_pipes[i][0] = -1;
            heredoc_pipes[i][1] = -1;
        }
        for (int i = 0; i < p->ncommands; i++) {
            Command *cmd = &p->commands[i];
            if (!cmd->heredoc_content) continue;

            if (pipe(heredoc_pipes[i]) < 0) {
                perror("pipe");
                continue;
            }

            /* expand and write heredoc content */
            extern char *expand_word(const char *word, int last_exit);
            extern int last_exit_status;
            const char *content = cmd->heredoc_content;
            char *expanded = NULL;
            if (cmd->heredoc_expand) {
                expanded = expand_word(content, last_exit_status);
                if (expanded) content = expanded;
            }
            size_t hlen = strlen(content);
            size_t written = 0;
            while (written < hlen) {
                ssize_t w = write(heredoc_pipes[i][1],
                                  content + written,
                                  hlen - written);
                if (w <= 0) break;
                written += w;
            }
            close(heredoc_pipes[i][1]);
            heredoc_pipes[i][1] = -1;
            free(expanded);
        }
        
        for (int i = 0; i < p->ncommands - 1; i++) {
            pipes[i] = malloc(2 * sizeof(int));
            if (!pipes[i]) {
                perror("malloc");
                // Free previously allocated pipes
                for (int j = 0; j < i; j++) {
                    free(pipes[j]);
                }
                free(pipes);
                return -1;
            }
            if (pipe(pipes[i]) < 0) {
                perror("pipe");
                // Free previously allocated pipes
                for (int j = 0; j <= i; j++) {
                    free(pipes[j]);
                }
                free(pipes);
                return -1;
            }
        }
        
        pid_t *pids = malloc(p->ncommands * sizeof(pid_t));
        if (!pids) {
            perror("malloc");
            // Free pipes
            for (int i = 0; i < p->ncommands - 1; i++) {
                free(pipes[i]);
            }
            free(pipes);
            return -1;
        }
        
        pid_t pgid = 0;
        
        // Fork children for each command
        for (int i = 0; i < p->ncommands; i++) {
            Command *cmd = &p->commands[i];
            pid_t pid = fork();
            
            if (pid == 0) {
                // Child process
                signals_child();
                
                // Set process group ID
                if (i == 0) {
                    pgid = getpid();
                }
                setpgid(0, pgid);
                
                /* Handle stdin redirection */
                /* heredoc pipe takes priority over regular pipe */
                if (heredoc_pipes[i][0] >= 0) {
                    dup2(heredoc_pipes[i][0], STDIN_FILENO);
                    close(heredoc_pipes[i][0]);
                } else if (i > 0) {
                    /* Connect previous pipe's read end to stdin */
                    dup2(pipes[i-1][0], STDIN_FILENO);
                    close(pipes[i-1][0]);
                } else if (cmd->infile) {
                    /* Handle infile for first command when no heredoc */
                    int fd = open(cmd->infile, O_RDONLY);
                    if (fd < 0) {
                        perror(cmd->infile);
                        _exit(1);
                    }
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                }
                
                /* also close all heredoc pipe read ends in child */
                for (int k = 0; k < p->ncommands; k++) {
                    if (k != i && heredoc_pipes[k][0] >= 0)
                        close(heredoc_pipes[k][0]);
                }
                
                /* Handle stdout redirection */
                if (i < p->ncommands - 1) {
                    /* Connect current pipe's write end to stdout */
                    dup2(pipes[i][1], STDOUT_FILENO);
                    close(pipes[i][1]);
                } else if (cmd->outfile) {
                    /* Handle outfile for last command */
                    int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
                    int fd = open(cmd->outfile, flags, 0644);
                    if (fd < 0) {
                        perror(cmd->outfile);
                        _exit(1);
                    }
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }
                
                
                // Close all pipe file descriptors
                for (int j = 0; j < p->ncommands - 1; j++) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                
                // Execute the command
                execvp(cmd->argv[0], cmd->argv);
                perror(cmd->argv[0]);
                _exit(127);
            } else if (pid > 0) {
                pids[i] = pid;
                
                // Set process group ID in parent (race condition fix)
                if (i == 0) {
                    pgid = pid;
                }
                setpgid(pid, pgid);
            } else {
                perror("fork");
                // Close all pipe file descriptors
                for (int j = 0; j < p->ncommands - 1; j++) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                // Free memory
                for (int j = 0; j < p->ncommands - 1; j++) {
                    free(pipes[j]);
                }
                free(pipes);
                free(pids);
                return -1;
            }
        }
        
        // Parent process - close all pipe file descriptors
        for (int i = 0; i < p->ncommands - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
            free(pipes[i]);
        }
        /* close all heredoc pipe read ends */
        for (int i = 0; i < p->ncommands; i++) {
            if (heredoc_pipes[i][0] >= 0) {
                close(heredoc_pipes[i][0]);
                heredoc_pipes[i][0] = -1;
            }
        }
        free(pipes);
        
        // Handle background pipeline
        if (p->background) {
            // Build command string for job
            char cmd_str[256] = {0};
            for (int i = 0; i < p->commands[0].argc && i < 10; i++) {  // Limit to prevent overflow
                if (i > 0) strcat(cmd_str, " ");
                strcat(cmd_str, p->commands[0].argv[i]);
            }
            
            int job_id = job_add(pgid, cmd_str);
            printf("[%d] %d\n", job_id, pgid);
            free(pids);
            return 0;
        }
        
        // Foreground pipeline
        // Give terminal control to child process group
        if (!g_in_procsubst) tcsetpgrp(STDIN_FILENO, pgid);
        if (!g_in_procsubst) set_fg_pid(pgid);
        
        // Wait for children
        int status = 0;
        int last_status = 0;
        int done = 0;
        int stopped = 0;

        while (done + stopped < p->ncommands) {
            pid_t w = waitpid(-pgid, &status, WUNTRACED);

            if (w == -1 && errno == EINTR) {
                if ((g_sigint_received || g_interrupt_loop) && pgid > 0) {
                    kill(-pgid, SIGINT);
                }
                continue;
            }
            if (w <= 0) break;

            if (WIFSTOPPED(status)) {
                stopped++;
            } else {
                done++;
                if (WIFEXITED(status))
                    last_status = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    last_status = 128 + WTERMSIG(status);
            }
        }

        set_fg_pid(0);
        if (!g_in_procsubst) tcsetpgrp(STDIN_FILENO, getpgrp());

        if (g_sigint_received || g_interrupt_loop) {
            write(STDOUT_FILENO, "\n", 1);
            return 130;
        }

        if (stopped > 0 && done < p->ncommands) {
            char cmd_str[256] = {0};
            for (int i = 0; i < p->commands[0].argc && i < 10; i++) {
                if (i > 0) strncat(cmd_str, " ",
                                   sizeof(cmd_str)-strlen(cmd_str)-1);
                strncat(cmd_str, p->commands[0].argv[i],
                        sizeof(cmd_str)-strlen(cmd_str)-1);
            }
            int job_id = job_add(pgid, cmd_str);
            job_set_status(pgid, JOB_STOPPED);
            write(STDOUT_FILENO, "\n", 1);
            printf("[%d]+  Stopped\t\t%s\n", job_id, cmd_str);
        }

        free(pids);
        return last_status;

    }
}
