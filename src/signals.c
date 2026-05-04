//
// Created by mete on 23.04.2026.
//

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/jobs.h"

extern int ps_pid_forget(pid_t pid);

int last_exit_status = 0;

static volatile sig_atomic_t fg_pid = 0;

void set_fg_pid(pid_t pid) {
    fg_pid = pid;
}

static void sigint_handler(int sig) {
    (void)sig;
    // Only reset prompt if no foreground process is running
    if (fg_pid == 0) {
        // Cannot use printf in signal handler, using write instead
        write(STDOUT_FILENO, "\nmysh> ", 7);
    }
}

static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    
    // Loop through all pending child signals
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        // Check if this is a process substitution child we should ignore
        if (ps_pid_forget(pid)) {
            continue;
        }

        // Check if this pid belongs to a known job or is the foreground process
        if (!job_find_by_pgid(pid) && pid != fg_pid) {
            // Unknown pid, just reap it silently
            continue;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            // Child exited normally or was terminated by a signal
            job_remove(pid);
            if (pid == fg_pid) {
                fg_pid = 0;
            }
            // Store exit status
            if (WIFEXITED(status)) {
                last_exit_status = WEXITSTATUS(status);
            } else {
                last_exit_status = 128 + WTERMSIG(status);
            }
        } else if (WIFSTOPPED(status)) {
            // Child was stopped
            job_set_status(pid, JOB_STOPPED);
            if (pid == (pid_t)fg_pid) fg_pid = 0;
            /* write is async-signal-safe, use it to print stopped notice */
            const char *msg = "\n[stopped]\n";
            write(STDOUT_FILENO, msg, 11);
        }
    }
}

void signals_init(void) {
    struct sigaction sa;
    
    // SIGINT handler
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    
    // Ignore SIGQUIT
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGQUIT, &sa, NULL);
    
    // Ignore SIGTSTP
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTSTP, &sa, NULL);
    
    // Ignore SIGTTOU
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTTOU, &sa, NULL);
    
    // Ignore SIGTTIN
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTTIN, &sa, NULL);
    
    // SIGCHLD handler
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
}

void signals_child(void) {
    // Put child in its own process group FIRST
    setpgid(0, 0);
    
    // Reset signal handlers to default
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
}
