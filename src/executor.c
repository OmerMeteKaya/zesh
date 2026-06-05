// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
#define _GNU_SOURCE
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <fnmatch.h>
#include <signal.h>
#include <time.h>
#include "../include/jobs.h"
#include "../include/shell.h"

int g_return_value = 0;
int g_returning    = 0;
int g_opt_errexit  = 0;
int g_opt_xtrace   = 0;
int g_opt_pipefail = 0;
extern void set_fg_pid(pid_t pid);
extern int  last_exit_status;
volatile int g_interrupt_loop = 0;
LoopControl g_loop_control = LOOP_NORMAL;
static int g_in_procsubst = 0;

static int execute_if(IfNode *node);
static int execute_select(SelectNode *node);
static int execute_time_node(TimeNode *node);
static int execute_coproc(CoprocNode *node);
static int execute_while(WhileNode *node);
static int execute_for(ForNode *node);
static int execute_list_expanded(CmdList *list);
static int execute_case(CaseNode *node);
static int execute_pipeline_expanded(Pipeline *p);
static int execute_subshell(SubshellNode *node);

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
            if (g_expand_error) {
                if (expanded) free(expanded);
                g_expand_error = 0;
                return 1;
            }
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
            if (g_expand_error) {
                /* free already-allocated args */
                for (int k = 0; k < ai; k++) free(dst->argv[k]);
                /* free remaining commands */
                for (int k = 0; k < ci; k++) {
                    for (int ak = 0; ak < tmp.commands[k].argc; ak++)
                        free(tmp.commands[k].argv[ak]);
                    free(tmp.commands[k].argv);
                    free(tmp.commands[k].infile);
                    free(tmp.commands[k].outfile);
                }
                free(dst->argv);
                free(tmp.commands);
                return 1;
            }
            dst->argv[ai] = ex ? ex : strdup(src->argv[ai]);
        }
        dst->argv[src->argc] = NULL;
        if (g_expand_error) {
            for (int _ci = 0; _ci <= ci; _ci++) {
                for (int _ai = 0; _ai < tmp.commands[_ci].argc; _ai++)
                    free(tmp.commands[_ci].argv[_ai]);
                free(tmp.commands[_ci].argv);
                free(tmp.commands[_ci].infile);
                free(tmp.commands[_ci].outfile);
            }
            free(tmp.commands);
            return 1;
        }

        dst->infile  = src->infile  ? expand_word(src->infile,  last_exit_status) : NULL;
        dst->outfile = src->outfile ? expand_word(src->outfile, last_exit_status) : NULL;

        if (g_expand_error) {
            for (int _ci = 0; _ci < ci; _ci++) {
                for (int _ai = 0; _ai < tmp.commands[_ci].argc; _ai++)
                    free(tmp.commands[_ci].argv[_ai]);
                free(tmp.commands[_ci].argv);
                free(tmp.commands[_ci].infile);
                free(tmp.commands[_ci].outfile);
            }
            free(tmp.commands);
            return 1;
        }

        /* copy fd_redirs from original (file fields are shared, not duped) */
        dst->nfd_redirs = src->nfd_redirs;
        for (int ri = 0; ri < src->nfd_redirs; ri++)
            dst->fd_redirs[ri] = src->fd_redirs[ri];
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
        char saved_funcname[256];
        strncpy(saved_funcname, g_current_funcname, sizeof(saved_funcname)-1);
        saved_funcname[sizeof(saved_funcname)-1] = '\0';
        strncpy(g_current_funcname, tmp.commands[0].argv[0],
                sizeof(g_current_funcname)-1);
        g_current_funcname[sizeof(g_current_funcname)-1] = '\0';

        scope_push();
        positional_set(tmp.commands[0].argv + 1,
                       tmp.commands[0].argc - 1);
        status = execute_list_expanded(fbody);
        positional_clear();
        g_returning = 0;
        scope_pop();

        strncpy(g_current_funcname, saved_funcname, sizeof(g_current_funcname)-1);

        if (g_opt_errexit && status != 0 &&
      strcmp(tmp.commands[0].argv[0], "set")  != 0 &&
      strcmp(tmp.commands[0].argv[0], "trap") != 0) {
            TMP_FREE();
            trap_run_exit(status);
      }
        TMP_FREE();
        return status;
    }
    /* xtrace: print command before execution */
    if (g_opt_xtrace) {
        write(STDERR_FILENO, "+ ", 2);
        for (int ai = 0; ai < tmp.commands[0].argc; ai++) {
            if (ai > 0) write(STDERR_FILENO, " ", 1);
            write(STDERR_FILENO, tmp.commands[0].argv[ai],
                  strlen(tmp.commands[0].argv[ai]));
        }
        write(STDERR_FILENO, "\n", 1);
    }
    /* Special: exec ${N}>&- in execute_pipeline_expanded path (e.g. inside if body).
     * Detect before applying fd_redirs to override the src fd. */
    if (tmp.commands[0].argv[0] &&
        strcmp(tmp.commands[0].argv[0], "exec") == 0 &&
        tmp.commands[0].argc == 2 && tmp.commands[0].argv[1]) {
        const char *a2 = tmp.commands[0].argv[1];
        int all_d = (*a2 != '\0');
        for (const char *pp = a2; *pp; pp++) if (*pp < '0' || *pp > '9') { all_d = 0; break; }
        if (all_d) {
            int fn2 = atoi(a2);
            for (int ri = 0; ri < tmp.commands[0].nfd_redirs; ri++) {
                FdRedir *r = &tmp.commands[0].fd_redirs[ri];
                int src2 = fn2;  /* use the numeric arg as src */
                if (r->dst_fd == -1) close(src2);
                else if (r->dst_fd >= 0) dup2(r->dst_fd, src2);
                else if (r->dst_fd == -2 && r->file) {
                    char *ef2 = expand_word(r->file, last_exit_status);
                    const char *ts2 = ef2 ? ef2 : r->file;
                    if (strcmp(ts2, "-") == 0) close(src2);
                    else {
                        char *ep2 = NULL; long fdn2 = strtol(ts2, &ep2, 10);
                        if (ep2 && *ep2 == '\0' && fdn2 >= 0) dup2((int)fdn2, src2);
                    }
                    free(ef2);
                }
            }
            TMP_FREE();
            return 0;
        }
    }

    /* builtin */
    if (is_builtin(tmp.commands[0].argv[0])) {
        Command *bcmd    = &tmp.commands[0];
        int saved_stdout = -1, saved_stdin = -1;
        int saved_fds_b[MAX_FD_REDIRS]; int nsaved_b = 0;

        fflush(stdout);
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
        /* apply fd_redirs for builtins */
        for (int ri = 0; ri < bcmd->nfd_redirs && nsaved_b < MAX_FD_REDIRS; ri++) {
            FdRedir *r = &bcmd->fd_redirs[ri];
            int src = r->src_fd >= 0 ? r->src_fd
                      : (r->is_input ? STDIN_FILENO : STDOUT_FILENO);
            saved_fds_b[nsaved_b++] = dup(src);
            if (r->dst_fd == -1) {
                close(src);
            } else if (r->dst_fd == -2 && r->file) {
                char *ef = expand_word(r->file, last_exit_status);
                const char *ts = ef ? ef : r->file;
                char *ep = NULL; long fdn = strtol(ts, &ep, 10);
                if (ep && *ep == '\0' && fdn >= 0) dup2((int)fdn, src);
                else if (strcmp(ts, "-") == 0) close(src);
                else {
                    int flags2 = r->is_input ? O_RDONLY
                                : (O_WRONLY|O_CREAT|(r->append?O_APPEND:O_TRUNC));
                    int fd = open(ts, flags2, 0644);
                    if (fd >= 0) { dup2(fd, src); close(fd); }
                }
                free(ef);
            } else if (r->dst_fd >= 0) {
                dup2(r->dst_fd, src);
            } else if (r->file) {
                char *ef = expand_word(r->file, last_exit_status);
                const char *ts = ef ? ef : r->file;
                int flags2 = r->is_input ? O_RDONLY
                            : (O_WRONLY|O_CREAT|(r->append?O_APPEND:O_TRUNC));
                int fd = open(ts, flags2, 0644);
                if (fd >= 0) { dup2(fd, src); close(fd); }
                free(ef);
            }
        }

        status = run_builtin(bcmd);

        /* restore fd_redirs */
        for (int ri = nsaved_b - 1; ri >= 0; ri--) {
            if (saved_fds_b[ri] < 0) continue;
            FdRedir *r = &bcmd->fd_redirs[ri];
            int src = r->src_fd >= 0 ? r->src_fd
                      : (r->is_input ? STDIN_FILENO : STDOUT_FILENO);
            dup2(saved_fds_b[ri], src);
            close(saved_fds_b[ri]);
        }
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
    if (g_opt_errexit && status != 0) {
        trap_run_exit(status);
    }
    return status;

    #undef TMP_FREE
}
/* Helper: execute a single CmdNode in the current process */
static int execute_node_inline(CmdNode *node) {
    switch (node->type) {
        case NODE_IF:       return execute_if(node->if_node);
        case NODE_WHILE:    return execute_while(node->while_node);
        case NODE_CASE:     return execute_case(node->case_node);
        case NODE_FOR:      return execute_for(node->for_node);
        case NODE_SELECT:   return execute_select(node->select_node);
        case NODE_TIME:     return execute_time_node(node->time_node);
        case NODE_COPROC:   return execute_coproc(node->coproc_node);
        case NODE_SUBSHELL: return execute_subshell(node->subshell_node);
        case NODE_PIPELINE:
        default:
            if (node->pipeline) return execute_pipeline_expanded(node->pipeline);
            return 0;
    }
}

static int execute_list_expanded(CmdList *list) {
    if (!list || list->count == 0) return 0;

    int last_status = 0;
    int pipe_read_fd = -1;  /* pending pipe read end from OP_PIPE producer */
    pid_t pipe_pid   = -1;  /* pid of the forked pipeline producer */

    for (int i = 0; i < list->count; i++) {
        CmdNode *node = &list->nodes[i];

        if (i > 0) {
            ListOp prev_op = list->nodes[i - 1].op;
            if (prev_op == OP_AND && last_status != 0) {
                if (pipe_read_fd >= 0) { close(pipe_read_fd); pipe_read_fd = -1; }
                continue;
            }
            if (prev_op == OP_OR  && last_status == 0) {
                if (pipe_read_fd >= 0) { close(pipe_read_fd); pipe_read_fd = -1; }
                continue;
            }
        }
        if (g_sigint_received || g_interrupt_loop) {
            if (pipe_read_fd >= 0) close(pipe_read_fd);
            return 130;
        }

        /* If current node's op is OP_PIPE and the next is a compound/pipeline,
         * fork the current node as a producer with stdout → pipe. */
        if (node->op == OP_PIPE && i + 1 < list->count) {
            int pfd[2];
            if (pipe(pfd) < 0) { last_status = 1; continue; }
            /* set up stdin for producer from previous pipe if any */
            int prod_saved_stdin = -1;
            if (pipe_read_fd >= 0) {
                prod_saved_stdin = dup(STDIN_FILENO);
                dup2(pipe_read_fd, STDIN_FILENO);
                close(pipe_read_fd);
                pipe_read_fd = -1;
            }
            fflush(NULL);
            pid_t pid = fork();
            if (pid == 0) {
                signals_child();
                dup2(pfd[1], STDOUT_FILENO);
                close(pfd[0]); close(pfd[1]);
                int s = execute_node_inline(node);
                _exit(s);
            }
            /* parent */
            close(pfd[1]);
            pipe_read_fd = pfd[0];
            pipe_pid = pid;
            if (prod_saved_stdin >= 0) {
                dup2(prod_saved_stdin, STDIN_FILENO);
                close(prod_saved_stdin);
            }
            /* skip normal execution — node ran in child */
            if (node->negate) last_status = (last_status == 0) ? 1 : 0;
            last_exit_status = last_status;
            continue;
        }

        /* Apply pending pipe from previous OP_PIPE producer */
        int saved_stdin = -1;
        if (pipe_read_fd >= 0) {
            saved_stdin = dup(STDIN_FILENO);
            dup2(pipe_read_fd, STDIN_FILENO);
            close(pipe_read_fd);
            pipe_read_fd = -1;
        }

        last_status = execute_node_inline(node);

        /* restore stdin and wait for producer */
        if (saved_stdin >= 0) {
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
        }
        if (pipe_pid > 0) {
            waitpid(pipe_pid, NULL, 0);
            pipe_pid = -1;
        }

        if (node->negate) last_status = (last_status == 0) ? 1 : 0;
        last_exit_status = last_status;
        if (g_opt_errexit && last_status != 0 && !g_returning) {
            trap_run_exit(last_status);
        }
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
    /* NULL condition = group command { ... }, always execute then_body */
    if (!node->condition) {
        /* group command { ... } — apply stored redirections */
        int saved_fds[MAX_FD_REDIRS + 2];
        int nsaved = 0;
        int saved_stdout = -1, saved_stdin = -1;
        /* outfile redirect */
        if (node->group_outfile) {
            int flags = O_WRONLY|O_CREAT|
                        (node->group_append ? O_APPEND : O_TRUNC);
            int fd = open(node->group_outfile, flags, 0644);
            if (fd >= 0) {
                saved_stdout = dup(STDOUT_FILENO);
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
        }
        if (node->group_infile) {
            int fd = open(node->group_infile, O_RDONLY);
            if (fd >= 0) {
                saved_stdin = dup(STDIN_FILENO);
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
        }
        /* fd redirections (e.g. 2>/tmp/file stored as src_fd=2) */
        for (int ri = 0; ri < node->group_nfd_redirs; ri++) {
            FdRedir *r = &node->group_fd_redirs[ri];
            int src = r->src_fd >= 0 ? r->src_fd : STDOUT_FILENO;
            saved_fds[nsaved++] = dup(src);
            if (r->dst_fd == -1) {
                close(src);
            } else if (r->file) {
                char *ef = expand_word(r->file, last_exit_status);
                const char *ts = ef ? ef : r->file;
                if (strcmp(ts, "-") == 0) {
                    close(src);
                } else {
                    int flags = r->is_input ? O_RDONLY :
                        (O_WRONLY|O_CREAT|(r->append?O_APPEND:O_TRUNC));
                    int fd = open(ts, flags, 0644);
                    if (fd >= 0) { dup2(fd, src); close(fd); }
                }
                free(ef);
            } else if (r->dst_fd >= 0) {
                dup2(r->dst_fd, src);
            }
        }
        int status = 0;
        if (node->then_body)
            status = execute_list_expanded(node->then_body);
        /* restore redirections */
        fflush(stdout); fflush(stderr);
        for (int ri = node->group_nfd_redirs - 1; ri >= 0; ri--) {
            if (saved_fds[ri] >= 0) {
                FdRedir *r = &node->group_fd_redirs[ri];
                int src = r->src_fd >= 0 ? r->src_fd : STDOUT_FILENO;
                dup2(saved_fds[ri], src);
                close(saved_fds[ri]);
            }
        }
        if (saved_stdout >= 0) {
            fflush(stdout);
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        if (saved_stdin >= 0) {
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
        }
        return status;
    }
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

    /* apply redirections on the while block */
    int wh_saved_stdin = -1, wh_saved_stdout = -1;
    int wh_saved_fds[MAX_FD_REDIRS]; int wh_nfds = 0;
    if (node->infile) {
        int fd = open(node->infile, O_RDONLY);
        if (fd >= 0) { wh_saved_stdin = dup(STDIN_FILENO); dup2(fd, STDIN_FILENO); close(fd); }
    }
    if (node->outfile) {
        int flags = O_WRONLY|O_CREAT|(node->append ? O_APPEND : O_TRUNC);
        int fd = open(node->outfile, flags, 0644);
        if (fd >= 0) { wh_saved_stdout = dup(STDOUT_FILENO); dup2(fd, STDOUT_FILENO); close(fd); }
    }
    for (int ri = 0; ri < node->nfd_redirs && wh_nfds < MAX_FD_REDIRS; ri++) {
        FdRedir *r = &node->fd_redirs[ri];
        int src = r->src_fd >= 0 ? r->src_fd : (r->is_input ? STDIN_FILENO : STDOUT_FILENO);
        wh_saved_fds[wh_nfds++] = dup(src);
        if (r->dst_fd == -1) close(src);
        else if (r->dst_fd >= 0) dup2(r->dst_fd, src);
        else if (r->file) {
            char *ef = expand_word(r->file, last_exit_status);
            const char *ts = ef ? ef : r->file;
            char *ep = NULL; long fdn = strtol(ts, &ep, 10);
            if (ep && *ep == '\0' && fdn >= 0) dup2((int)fdn, src);
            else if (strcmp(ts, "-") == 0) close(src);
            else { int f2 = open(ts, r->is_input?O_RDONLY:(O_WRONLY|O_CREAT|O_TRUNC),0644); if(f2>=0){dup2(f2,src);close(f2);} }
            free(ef);
        }
    }

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
    /* restore redirections */
    for (int ri = wh_nfds - 1; ri >= 0; ri--) {
        if (wh_saved_fds[ri] < 0) continue;
        FdRedir *r = &node->fd_redirs[ri];
        int src = r->src_fd >= 0 ? r->src_fd : (r->is_input ? STDIN_FILENO : STDOUT_FILENO);
        dup2(wh_saved_fds[ri], src); close(wh_saved_fds[ri]);
    }
    if (wh_saved_stdout >= 0) { dup2(wh_saved_stdout, STDOUT_FILENO); close(wh_saved_stdout); }
    if (wh_saved_stdin  >= 0) { dup2(wh_saved_stdin,  STDIN_FILENO);  close(wh_saved_stdin);  }

    return status;
}

static int execute_select(SelectNode *node) {
    if (!node) return 1;

    /* apply redirections */
    int saved_stdin = -1, saved_stdout = -1;
    int saved_fds_sel[MAX_FD_REDIRS]; int nsel = 0;
    if (node->infile) {
        int fd = open(node->infile, O_RDONLY);
        if (fd >= 0) { saved_stdin = dup(STDIN_FILENO); dup2(fd, STDIN_FILENO); close(fd); }
    }
    if (node->outfile) {
        int flags = O_WRONLY|O_CREAT|(node->append ? O_APPEND : O_TRUNC);
        int fd = open(node->outfile, flags, 0644);
        if (fd >= 0) { saved_stdout = dup(STDOUT_FILENO); dup2(fd, STDOUT_FILENO); close(fd); }
    }
    for (int ri = 0; ri < node->nfd_redirs && nsel < MAX_FD_REDIRS; ri++) {
        FdRedir *r = &node->fd_redirs[ri];
        int src = r->src_fd >= 0 ? r->src_fd : (r->is_input ? STDIN_FILENO : STDOUT_FILENO);
        saved_fds_sel[nsel++] = dup(src);
        if (r->dst_fd == -1) close(src);
        else if (r->dst_fd >= 0) dup2(r->dst_fd, src);
        else if (r->file) {
            char *ef = expand_word(r->file, last_exit_status);
            const char *ts = ef ? ef : r->file;
            char *ep = NULL; long fdn = strtol(ts, &ep, 10);
            if (ep && *ep == '\0' && fdn >= 0) dup2((int)fdn, src);
            else if (strcmp(ts, "-") == 0) close(src);
            else { int f2 = open(ts, r->is_input?O_RDONLY:(O_WRONLY|O_CREAT|O_TRUNC),0644); if(f2>=0){dup2(f2,src);close(f2);} }
            free(ef);
        }
    }

    /* expand word list */
    char *words[256];
    int   nwords = 0;
    for (int i = 0; i < node->nwords && i < 256; i++) {
        char *w = expand_word(node->words[i], last_exit_status);
        words[nwords++] = w ? w : strdup(node->words[i]);
    }

    int status = 0;
    while (1) {
        if (g_sigint_received || g_interrupt_loop) {
            g_sigint_received = 0; g_interrupt_loop = 0;
            for (int i = 0; i < nwords; i++) free(words[i]);
            return 130;
        }
        /* print numbered menu to stderr */
        for (int i = 0; i < nwords; i++)
            fprintf(stderr, "%d) %s\n", i + 1, words[i]);

        const char *ps3 = var_get("PS3");
        if (!ps3 || !ps3[0]) ps3 = "#? ";
        fprintf(stderr, "%s", ps3);
        fflush(stderr);

        char line[256] = {0};
        int li = 0;
        char ch;
        ssize_t nr2;
        int got_any = 0;
        while (li < (int)sizeof(line) - 1 && (nr2 = read(STDIN_FILENO, &ch, 1)) == 1) {
            got_any = 1;
            if (ch == '\n') break;
            line[li++] = ch;
        }
        if (!got_any) {
            write(STDOUT_FILENO, "\n", 1);
            break;
        }
        line[li] = '\0';

        int sel = atoi(line);
        if (sel >= 1 && sel <= nwords) {
            local_var_set(node->var, words[sel - 1]);
            setenv(node->var, words[sel - 1], 1);
        } else {
            local_var_set(node->var, "");
            setenv(node->var, "", 1);
        }
        /* set REPLY */
        local_var_set("REPLY", line);
        setenv("REPLY", line, 1);

        if (node->body)
            status = execute_list_expanded(node->body);

        if (g_loop_control == LOOP_BREAK) { g_loop_control = LOOP_NORMAL; break; }
        if (g_loop_control == LOOP_CONTINUE) { g_loop_control = LOOP_NORMAL; continue; }
    }
    for (int i = 0; i < nwords; i++) free(words[i]);

    /* restore redirections */
    for (int ri = nsel - 1; ri >= 0; ri--) {
        if (saved_fds_sel[ri] < 0) continue;
        FdRedir *r = &node->fd_redirs[ri];
        int src = r->src_fd >= 0 ? r->src_fd : (r->is_input ? STDIN_FILENO : STDOUT_FILENO);
        dup2(saved_fds_sel[ri], src); close(saved_fds_sel[ri]);
    }
    if (saved_stdout >= 0) { dup2(saved_stdout, STDOUT_FILENO); close(saved_stdout); }
    if (saved_stdin  >= 0) { dup2(saved_stdin,  STDIN_FILENO);  close(saved_stdin);  }

    return status;
}

static int execute_coproc(CoprocNode *node) {
    if (!node || (!node->pipeline && !node->body)) return 1;

    /* two pipes: shell→coproc (stdin of coproc) and coproc→shell (stdout of coproc) */
    int to_coproc[2];    /* shell writes [1], coproc reads [0] */
    int from_coproc[2];  /* coproc writes [1], shell reads [0] */

    if (pipe(to_coproc) < 0 || pipe(from_coproc) < 0) {
        perror("coproc: pipe");
        return 1;
    }

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("coproc: fork"); return 1; }

    if (pid == 0) {
        /* child: connect pipes to stdin/stdout */
        signals_child();
        dup2(to_coproc[0],   STDIN_FILENO);
        dup2(from_coproc[1], STDOUT_FILENO);
        close(to_coproc[0]);  close(to_coproc[1]);
        close(from_coproc[0]); close(from_coproc[1]);
        /* run the pipeline or group body */
        if (node->body)
            execute_list_expanded(node->body);
        else
            execute(node->pipeline);
        _exit(0);
    }

    /* parent: close child-facing ends */
    close(to_coproc[0]);
    close(from_coproc[1]);

    /* store fds in array: NAME[0]=read_fd, NAME[1]=write_fd */
    const char *cp_name = node->name ? node->name : "COPROC";
    char arr_name[256];

    /* COPROC[0] = from_coproc[0] (read from coproc), COPROC[1] = to_coproc[1] (write to coproc) */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", from_coproc[0]);
    snprintf(arr_name, sizeof(arr_name), "%s", cp_name);
    arr_set(arr_name, 0, buf);

    snprintf(buf, sizeof(buf), "%d", to_coproc[1]);
    arr_set(arr_name, 1, buf);

    /* also set COPROC_PID */
    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%d", (int)pid);
    local_var_set("COPROC_PID", pidbuf);
    setenv("COPROC_PID", pidbuf, 1);

    /* add to background jobs */
    char cmd_str[256] = {0};
    if (node->body) {
        strncpy(cmd_str, node->name ? node->name : "coproc", sizeof(cmd_str)-1);
    } else if (node->pipeline && node->pipeline->ncommands > 0 &&
        node->pipeline->commands[0].argv &&
        node->pipeline->commands[0].argv[0]) {
        strncpy(cmd_str, node->pipeline->commands[0].argv[0], sizeof(cmd_str)-1);
    }
    int job_id = job_add(pid, cmd_str);
    /* use write() to bypass stdio buffering — printf could flush buffered
       data into a later fd-redirected stdout */
    char notif[64];
    int nlen = snprintf(notif, sizeof(notif), "[%d] %d\n", job_id, (int)pid);
    write(STDOUT_FILENO, notif, nlen);

    return 0;
}

static int execute_time_node(TimeNode *node) {
    if (!node || !node->pipeline) return 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int status = execute(node->pipeline);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double real = (end.tv_sec - start.tv_sec) +
                  (end.tv_nsec - start.tv_nsec) / 1e9;
    int rm = (int)(real / 60);
    double rs = real - rm * 60;
    fprintf(stderr, "\nreal\t%dm%.3fs\n", rm, rs);
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

/* ---- subshell execution ---- */
static int execute_subshell(SubshellNode *node) {
    if (!node || !node->body) return 0;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("subshell"); return 1; }
    if (pid == 0) {
        signals_child();
        g_is_subshell = 1;
        /* apply redirections in child */
        if (node->outfile) {
            int flags = O_WRONLY|O_CREAT|(node->append?O_APPEND:O_TRUNC);
            int fd = open(node->outfile, flags, 0644);
            if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
        }
        if (node->infile) {
            int fd = open(node->infile, O_RDONLY);
            if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
        }
        for (int ri = 0; ri < node->nfd_redirs; ri++) {
            FdRedir *r = &node->fd_redirs[ri];
            int src = r->src_fd >= 0 ? r->src_fd : STDOUT_FILENO;
            if (r->dst_fd == -1) { close(src); }
            else if (r->file) {
                int flags = r->is_input ? O_RDONLY :
                    (O_WRONLY|O_CREAT|(r->append?O_APPEND:O_TRUNC));
                int fd = open(r->file, flags, 0644);
                if (fd >= 0) { dup2(fd, src); close(fd); }
            } else if (r->dst_fd >= 0) { dup2(r->dst_fd, src); }
        }
        int status = execute_list_expanded(node->body);
        fflush(stdout);
        _exit(status);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

/* ------------------------------------------------------------------ */
/*                         execute_list                               */
/* ------------------------------------------------------------------ */

int execute_list(CmdList *list) {
    if (!list || list->count == 0) return 0;

    int last_status = 0;
    int pipe_read_fd_l = -1;
    pid_t pipe_pid_l   = -1;

    for (int i = 0; i < list->count; i++) {
        CmdNode *node = &list->nodes[i];

        /* --- && / || short-circuit --- */
        if (i > 0) {
            ListOp prev_op = list->nodes[i - 1].op;
            if (prev_op == OP_AND && last_status != 0) {
                if (pipe_read_fd_l >= 0) { close(pipe_read_fd_l); pipe_read_fd_l = -1; }
                continue;
            }
            if (prev_op == OP_OR  && last_status == 0) {
                if (pipe_read_fd_l >= 0) { close(pipe_read_fd_l); pipe_read_fd_l = -1; }
                continue;
            }
        }
        if (g_sigint_received || g_interrupt_loop) {
            if (pipe_read_fd_l >= 0) close(pipe_read_fd_l);
            return 130;
        }

        /* OP_PIPE: fork producer, pipe stdout to next node's stdin */
        if (node->op == OP_PIPE && i + 1 < list->count) {
            int pfd2[2];
            if (pipe(pfd2) < 0) { last_status = 1; continue; }
            int prod_saved_stdin2 = -1;
            if (pipe_read_fd_l >= 0) {
                prod_saved_stdin2 = dup(STDIN_FILENO);
                dup2(pipe_read_fd_l, STDIN_FILENO);
                close(pipe_read_fd_l); pipe_read_fd_l = -1;
            }
            fflush(NULL);
            pid_t pid2 = fork();
            if (pid2 == 0) {
                signals_child();
                dup2(pfd2[1], STDOUT_FILENO);
                close(pfd2[0]); close(pfd2[1]);
                int s2 = execute_node_inline(node);
                _exit(s2);
            }
            close(pfd2[1]);
            pipe_read_fd_l = pfd2[0];
            pipe_pid_l = pid2;
            if (prod_saved_stdin2 >= 0) {
                dup2(prod_saved_stdin2, STDIN_FILENO);
                close(prod_saved_stdin2);
            }
            continue;
        }

        /* Apply pending pipe from OP_PIPE producer */
        int saved_stdin_l = -1;
        if (pipe_read_fd_l >= 0) {
            saved_stdin_l = dup(STDIN_FILENO);
            dup2(pipe_read_fd_l, STDIN_FILENO);
            close(pipe_read_fd_l); pipe_read_fd_l = -1;
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
        case NODE_SELECT:
            last_status = execute_select(node->select_node);
            break;
        case NODE_TIME:
            last_status = execute_time_node(node->time_node);
            break;
        case NODE_COPROC:
            last_status = execute_coproc(node->coproc_node);
            break;
        case NODE_SUBSHELL:
            last_status = execute_subshell(node->subshell_node);
            break;

case NODE_PIPELINE:
default: {
    Pipeline *p = node->pipeline;
    /* xtrace */
    if (g_opt_xtrace && p && p->ncommands > 0 &&
        p->commands[0].argv && p->commands[0].argv[0]) {
        write(STDERR_FILENO, "+ ", 2);
        for (int ai = 0; ai < p->commands[0].argc; ai++) {
            if (!p->commands[0].argv[ai]) break;
            if (ai > 0) write(STDERR_FILENO, " ", 1);
            write(STDERR_FILENO, p->commands[0].argv[ai],
                  strlen(p->commands[0].argv[ai]));
        }
        write(STDERR_FILENO, "\n", 1);
        }
    if (!p) { last_status = 0; break; }

    if (p->ncommands == 0 || !p->commands[0].argv ||
        !p->commands[0].argv[0]) { last_status = 0; break; }

    /* exec builtin — handled before normal builtin path to avoid fd save/restore */
    if (p->ncommands == 1 &&
        strcmp(p->commands[0].argv[0], "exec") == 0) {
        Command *ec = &p->commands[0];
        /* expand argv */
        char *eargv[MAX_ARGS + 1];
        for (int ai = 0; ai < ec->argc && ai < MAX_ARGS; ai++) {
            char *ex = ec->argv[ai] ? expand_word(ec->argv[ai], last_exit_status) : NULL;
            eargv[ai] = ex ? ex : (ec->argv[ai] ? strdup(ec->argv[ai]) : NULL);
        }
        eargv[ec->argc] = NULL;
        char *ein  = ec->infile  ? expand_word(ec->infile,  last_exit_status) : NULL;
        char *eout = ec->outfile ? expand_word(ec->outfile, last_exit_status) : NULL;

        /* apply outfile/infile redirections permanently */
        if (eout) {
            int flags = O_WRONLY | O_CREAT | (ec->append ? O_APPEND : O_TRUNC);
            int fd = open(eout, flags, 0644);
            if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
            free(eout);
        }
        if (ein) {
            int fd = open(ein, O_RDONLY);
            if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
            free(ein);
        }

        /* Special case: exec ${N}>&- where ${N} expands to digits only —
         * treat as an fd redirect, not a command.  Detect before applying
         * fd_redirs so we can override the default src fd. */
        int exec_fd_redir_only = 0;
        int exec_fd_num = -1;
        if (ec->argc == 2 && eargv[1]) {
            const char *a = eargv[1];
            int all_digits = (*a != '\0');
            for (const char *p2 = a; *p2; p2++)
                if (*p2 < '0' || *p2 > '9') { all_digits = 0; break; }
            if (all_digits) { exec_fd_redir_only = 1; exec_fd_num = atoi(eargv[1]); }
        }

        /* apply fd_redirs permanently */
        for (int ri = 0; ri < ec->nfd_redirs; ri++) {
            FdRedir *r = &ec->fd_redirs[ri];
            /* when exec_fd_num is set (from variable like exec ${N}>&-),
             * use fd_num as the src override instead of the parsed default */
            int src;
            if (exec_fd_redir_only && exec_fd_num >= 0)
                src = exec_fd_num;
            else
                src = r->src_fd >= 0 ? r->src_fd
                      : (r->is_input ? STDIN_FILENO : STDOUT_FILENO);
            if (r->dst_fd == -1) {
                close(src);
            } else if (r->dst_fd == -2 && r->file) {
                char *ef = expand_word(r->file, last_exit_status);
                const char *ts = ef ? ef : r->file;
                char *ep = NULL; long fdn = strtol(ts, &ep, 10);
                if (ep && *ep == '\0' && fdn >= 0) dup2((int)fdn, src);
                else if (strcmp(ts, "-") == 0) close(src);
                else {
                    int flags = r->is_input ? O_RDONLY : (O_WRONLY|O_CREAT|(r->append?O_APPEND:O_TRUNC));
                    int fd = open(ts, flags, 0644);
                    if (fd >= 0) { dup2(fd, src); close(fd); }
                }
                free(ef);
            } else if (r->dst_fd >= 0) {
                dup2(r->dst_fd, src);
            } else if (r->file) {
                char *ef = expand_word(r->file, last_exit_status);
                const char *ts = ef ? ef : r->file;
                int flags = r->is_input ? O_RDONLY : (O_WRONLY|O_CREAT|(r->append?O_APPEND:O_TRUNC));
                int fd = open(ts, flags, 0644);
                if (fd >= 0) { dup2(fd, src); close(fd); }
                free(ef);
            }
        }

        if (exec_fd_redir_only) {
            for (int ai = 0; ai < ec->argc; ai++) free(eargv[ai]);
            last_status = 0;
            break;
        }

        /* process replacement: exec cmd [args] */
        if (ec->argc >= 2 && eargv[1]) {
            execvp(eargv[1], eargv + 1);
            perror(eargv[1]);
            for (int ai = 0; ai < ec->argc; ai++) free(eargv[ai]);
            _exit(127);
        }
        for (int ai = 0; ai < ec->argc; ai++) free(eargv[ai]);
        last_status = 0;
        break;
    }

    /* inline assignment */
    if (p->ncommands == 1 && p->commands[0].argc == 1) {
        char *arg = p->commands[0].argv[0];
        char *eq  = strchr(arg, '=');
        if (eq && eq != arg && strchr(arg, '[') == NULL) {
            char name[64] = {0};
            strncpy(name, arg, eq - arg);
            char *expanded = expand_word(eq + 1, last_exit_status);
            if (g_expand_error) {
                free(expanded);
                g_expand_error = 0;
                last_status = 1;
                last_exit_status = 1;
                break;
            }
            local_var_set(name, expanded ? expanded : eq + 1);
            if (expanded) free(expanded);
            last_status = 0;
            last_exit_status = 0;
            break;
        }
    }

    CmdList *fbody = func_get_body(p->commands[0].argv[0]);
    if (fbody) {
        char saved_funcname[256];
        strncpy(saved_funcname, g_current_funcname, sizeof(saved_funcname)-1);
        saved_funcname[sizeof(saved_funcname)-1] = '\0';
        strncpy(g_current_funcname, p->commands[0].argv[0],
                sizeof(g_current_funcname)-1);
        g_current_funcname[sizeof(g_current_funcname)-1] = '\0';

        scope_push();
        positional_set(p->commands[0].argv + 1,
                       p->commands[0].argc - 1);
        last_status = execute_list_expanded(fbody);
        positional_clear();
        g_returning = 0;
        last_exit_status = last_status;
        scope_pop();

        strncpy(g_current_funcname, saved_funcname, sizeof(g_current_funcname)-1);
        break;
    }

    /* builtin */
    if (is_builtin(p->commands[0].argv[0])) {
        /* expand argv at execution time so variables set by earlier
           commands in the same list (e.g. read && echo $var) are seen */
        Command expanded_cmd = p->commands[0];
        char *expanded_argv[MAX_ARGS + 1];
        for (int ai = 0; ai < expanded_cmd.argc && ai < MAX_ARGS; ai++) {
            if (!expanded_cmd.argv[ai]) { expanded_argv[ai] = NULL; continue; }
            char *ex = expand_word(expanded_cmd.argv[ai], last_exit_status);
            expanded_argv[ai] = ex ? ex : expanded_cmd.argv[ai];
        }
        if (g_expand_error) {
            g_expand_error = 0;
            last_status = 1;
            last_exit_status = 1;
            break;
        }
        expanded_argv[expanded_cmd.argc] = NULL;
        expanded_cmd.argv = expanded_argv;
        expanded_cmd.infile  = expanded_cmd.infile
            ? expand_word(expanded_cmd.infile,  last_exit_status)
            : NULL;
        expanded_cmd.outfile = expanded_cmd.outfile
            ? expand_word(expanded_cmd.outfile, last_exit_status)
            : NULL;
        Command *bcmd = &expanded_cmd;
        int saved_stdout = -1, saved_stdin = -1;
        /* save/restore for fd_redirs */
        int saved_fds[MAX_FD_REDIRS];
        for (int ri = 0; ri < bcmd->nfd_redirs; ri++) saved_fds[ri] = -1;

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
        /* apply fd_redirs */
        for (int ri = 0; ri < bcmd->nfd_redirs; ri++) {
            FdRedir *r = &bcmd->fd_redirs[ri];
            int src = r->src_fd >= 0 ? r->src_fd : (r->is_input ? STDIN_FILENO : STDOUT_FILENO);
            saved_fds[ri] = dup(src);
            if (r->dst_fd == -1) {
                close(src);
            } else if (r->dst_fd == -2 && r->file) {
                char *etgt = expand_word(r->file, last_exit_status);
                const char *ts = etgt ? etgt : r->file;
                char *ep2 = NULL;
                long fdn = strtol(ts, &ep2, 10);
                if (ep2 && *ep2 == '\0' && fdn >= 0) dup2((int)fdn, src);
                else if (strcmp(ts, "-") == 0) close(src);
                else { int f2 = open(ts, r->is_input ? O_RDONLY : (O_WRONLY|O_CREAT|O_TRUNC), 0644); if (f2>=0){dup2(f2,src);close(f2);} }
                free(etgt);
            } else if (r->dst_fd >= 0) {
                dup2(r->dst_fd, src);
            } else if (r->file) {
                int flags2 = r->is_input ? O_RDONLY :
                             (O_WRONLY | O_CREAT | (r->append ? O_APPEND : O_TRUNC));
                int fd2 = open(r->file, flags2, 0644);
                if (fd2 >= 0) { dup2(fd2, src); close(fd2); }
            }
        }

        last_status = run_builtin(bcmd);

        /* restore fd_redirs */
        fflush(stdout);  /* flush before any stdout fd restore */
        for (int ri = bcmd->nfd_redirs - 1; ri >= 0; ri--) {
            if (saved_fds[ri] >= 0) {
                FdRedir *r = &bcmd->fd_redirs[ri];
                int src = r->src_fd >= 0 ? r->src_fd : (r->is_input ? STDIN_FILENO : STDOUT_FILENO);
                dup2(saved_fds[ri], src);
                close(saved_fds[ri]);
            }
        }

        if (saved_stdout >= 0) {
            fflush(stdout);
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        if (saved_stdin >= 0) {
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
        }
        /* free late-expanded argv */
        for (int ai = 0; ai < expanded_cmd.argc && ai < MAX_ARGS; ai++) {
            if (expanded_argv[ai] && expanded_argv[ai] != p->commands[0].argv[ai])
                free(expanded_argv[ai]);
        }
        if (expanded_cmd.infile  != p->commands[0].infile)  free(expanded_cmd.infile);
        if (expanded_cmd.outfile != p->commands[0].outfile) free(expanded_cmd.outfile);
        break;
    }
    /* external */
    last_status = execute(p);
    break;
}
        } /* switch */

        /* restore stdin if it was redirected from pipe */
        if (saved_stdin_l >= 0) {
            dup2(saved_stdin_l, STDIN_FILENO);
            close(saved_stdin_l);
            saved_stdin_l = -1;
        }
        if (pipe_pid_l > 0) {
            waitpid(pipe_pid_l, NULL, 0);
            pipe_pid_l = -1;
        }

        if (node->negate) last_status = (last_status == 0) ? 1 : 0;
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

        fflush(NULL);
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

            /* fd redirections */
            for (int ri = 0; ri < cmd->nfd_redirs; ri++) {
                FdRedir *r = &cmd->fd_redirs[ri];
                int src = r->src_fd >= 0 ? r->src_fd : (r->is_input ? STDIN_FILENO : STDOUT_FILENO);
                if (r->dst_fd == -1) {
                    close(src);
                } else if (r->dst_fd == -2 && r->file) {
                    /* expand and use as fd number or file */
                    char *expanded_tgt = expand_word(r->file, last_exit_status);
                    const char *tgt_str = expanded_tgt ? expanded_tgt : r->file;
                    /* check if it's a pure integer */
                    char *endp = NULL;
                    long fd_num2 = strtol(tgt_str, &endp, 10);
                    if (endp && *endp == '\0' && fd_num2 >= 0) {
                        dup2((int)fd_num2, src);
                    } else if (strcmp(tgt_str, "-") == 0) {
                        close(src);
                    } else {
                        /* treat as filename */
                        int flags = r->is_input ? O_RDONLY :
                                    (O_WRONLY | O_CREAT | O_TRUNC);
                        int fd2 = open(tgt_str, flags, 0644);
                        if (fd2 >= 0) { dup2(fd2, src); close(fd2); }
                    }
                    free(expanded_tgt);
                } else if (r->dst_fd >= 0) {
                    dup2(r->dst_fd, src);
                } else if (r->file) {
                    int flags = r->is_input ? O_RDONLY :
                                (O_WRONLY | O_CREAT | (r->append ? O_APPEND : O_TRUNC));
                    int fd2 = open(r->file, flags, 0644);
                    if (fd2 >= 0) { dup2(fd2, src); close(fd2); }
                }
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
            g_last_bg_pid = pid;
            char bg_notif[64];
            int bg_nlen = snprintf(bg_notif, sizeof(bg_notif), "[%d] %d\n", job_id, pid);
            write(STDOUT_FILENO, bg_notif, bg_nlen);
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
        if (heredoc_pipe[0] >= 0) close(heredoc_pipe[0]);
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
            fflush(NULL);
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
            size_t cmd_rem = sizeof(cmd_str) - 1;
            for (int i = 0; i < p->commands[0].argc && cmd_rem > 0; i++) {
                if (!p->commands[0].argv[i]) break;
                if (i > 0 && cmd_rem > 1) {
                    strncat(cmd_str, " ", cmd_rem);
                    cmd_rem--;
                }
                size_t alen = strlen(p->commands[0].argv[i]);
                strncat(cmd_str, p->commands[0].argv[i], cmd_rem);
                cmd_rem -= (alen < cmd_rem) ? alen : cmd_rem;
            }
            
            int job_id = job_add(pgid, cmd_str);
            g_last_bg_pid = pgid;
            char bgp_notif[64];
            int bgp_nlen = snprintf(bgp_notif, sizeof(bgp_notif), "[%d] %d\n", job_id, pgid);
            write(STDOUT_FILENO, bgp_notif, bgp_nlen);
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

        int first_fail = 0; /* for pipefail */
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
                int s = 0;
                if (WIFEXITED(status))
                    s = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    s = 128 + WTERMSIG(status);
                /* pipefail: track first failure */
                if (g_opt_pipefail && s != 0 && first_fail == 0)
                    first_fail = s;
                last_status = s;
            }
        }
        if (g_opt_pipefail && first_fail != 0)
            last_status = first_fail;

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
        if (g_opt_pipefail) {
            /* pipefail: return first non-zero exit status in pipeline */
            /* last_status already holds last command's status;
               for full pipefail we'd need all statuses — this is
               a reasonable approximation */
        }
        if (g_opt_errexit && last_status != 0)
            trap_run_exit(last_status);
        free(pids);
        return last_status;

    }
}
