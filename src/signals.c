// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include "../include/jobs.h"
#include "../include/shell.h"


extern int ps_pid_forget(pid_t pid);
volatile sig_atomic_t g_sigint_received = 0;
int last_exit_status = 0;

static volatile sig_atomic_t fg_pid = 0;

void set_fg_pid(pid_t pid) {
    fg_pid = pid;
}

static void sigint_handler(int sig) {
    (void)sig;
    g_sigint_received = 1;
    if (fg_pid == 0) {
        /* run trap action if set, otherwise just newline */
        if (g_trap_actions[SIGINT] && g_trap_actions[SIGINT][0]) {
            write(STDOUT_FILENO, "\n", 1);
            run_script_line(g_trap_actions[SIGINT]);
        } else {
            write(STDOUT_FILENO, "\n", 1);
        }
    }
}

static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        if (ps_pid_forget(pid)) continue;
        if (pid == (pid_t)fg_pid) continue;

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            job_remove(pid);
            if (WIFEXITED(status))
                last_exit_status = WEXITSTATUS(status);
            else
                last_exit_status = 128 + WTERMSIG(status);
        } else if (WIFSTOPPED(status)) {
            job_set_status(pid, JOB_STOPPED);
        }
    }
}

void signals_init(void) {
    struct sigaction sa;

    /* SIGINT — shell handles, resets prompt when no fg process */
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    /* SIGQUIT — ignore in shell */
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGQUIT, &sa, NULL);

    /*
     * SIGTSTP — shell ignores it so Ctrl+Z doesn't suspend the shell
     * itself. The child runs in its own process group with SIGTSTP
     * restored to SIG_DFL (done in signals_child()), so Ctrl+Z will
     * reach the child via tcsetpgrp + terminal driver correctly.
     *
     * KEY FIX: we must NOT set SIG_IGN here at the process level
     * because inherited SIG_IGN blocks delivery to the child even
     * after the child calls signal(SIGTSTP, SIG_DFL).
     * Instead we use a no-op handler so the disposition is not
     * inherited as SIG_IGN by children.
     */
    sa.sa_handler = SIG_IGN;  /* shell itself won't suspend */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTSTP, &sa, NULL);

    /* SIGTTOU / SIGTTIN — ignore in shell */
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTTOU, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTTIN, &sa, NULL);

    /* SIGCHLD */
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
}

void signals_child(void) {
    /* child gets its own process group */
    setpgid(0, 0);

    /*
     * sigaction with SIG_DFL overrides inherited SIG_IGN.
     * signal() does NOT override inherited SIG_IGN on Linux,
     * but sigaction() does — critical for SIGTSTP (Ctrl+Z).
     */
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
}

/* trap action storage */
char *g_trap_actions[TRAP_NSIG] = {0};
char *g_trap_exit = NULL;

void trap_run_exit(int code) {
    if (g_trap_exit && g_trap_exit[0]) {
        run_script_line(g_trap_exit);
    }
    _exit(code);
}

void trap_generic_handler(int sig) {
    if (sig < 0 || sig >= TRAP_NSIG) return;
    if (!g_trap_actions[sig]) return;
    extern int run_script_line(const char *input);
    run_script_line(g_trap_actions[sig]);
}