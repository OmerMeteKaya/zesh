// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// Fuzz target: expand_word().  Treats the input as a single "word" and runs
// the full expansion pipeline: tilde, parameter (${...}, ${x:-y}, ${x@U}…),
// arithmetic $((...)), command $(...) and process <(...) substitution.
// Good at finding overflows in parameter expansion and ANSI-C quoting.
//
// ============================ SAFETY WARNING ============================
//  expand_word() RESOLVES COMMAND/PROCESS SUBSTITUTION, which means a test
//  case like  $(rm -rf ~)  WILL FORK AND EXECUTE.  Never run this target on
//  untrusted input outside a throwaway sandbox (container / VM / disposable
//  user).  As a thin first line of defence the harness below clears PATH and
//  chdir()s into a temp dir so PATH-relative commands fail, but absolute
//  paths in a test case are NOT contained.  RUN IT IN A CONTAINER.
// =======================================================================

#include <unistd.h>
#include <stdlib.h>

#include "fuzz_common.h"
#include "../include/shell.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>

extern int g_opt_errexit;
extern int g_opt_xtrace;
extern int g_opt_pipefail;
extern int g_returning;
extern int g_return_value;
extern volatile int g_interrupt_loop;
extern int g_expand_error;
extern int g_is_subshell;
extern int g_current_lineno;
extern LoopControl g_loop_control;
extern volatile sig_atomic_t g_sigint_received;
extern char  g_current_funcname[256];
extern char  g_current_source[4096];
extern pid_t g_last_bg_pid;
extern char *g_trap_actions[TRAP_NSIG];
extern char *g_trap_exit;
extern int   g_declared_var_count;

static void reset_globals(void)
{
    g_opt_errexit     = 0;
    g_opt_xtrace      = 0;
    g_opt_pipefail    = 0;
    g_returning       = 0;
    g_return_value    = 0;
    g_interrupt_loop  = 0;
    g_expand_error    = 0;
    g_is_subshell     = 0;
    g_loop_control    = LOOP_NORMAL;
    g_sigint_received = 0;
    g_current_lineno  = 0;
    g_last_bg_pid     = 0;
    g_declared_var_count = 0;
    g_current_funcname[0] = '\0';
    g_current_source[0]   = '\0';
    for (int i = 0; i < TRAP_NSIG; i++) { free(g_trap_actions[i]); g_trap_actions[i] = NULL; }
    free(g_trap_exit); g_trap_exit = NULL;
    func_free_all();
    memset(g_declared_vars, 0, sizeof(g_declared_vars));
    g_shell_start_time = 1; /* FIX: fixed time prevents $SECONDS non-determinism */
}

static void on_alarm(int sig) { (void)sig; _exit(1); }

static void fuzz_setup(void)
{
    setenv("PATH", "", 1);
    srand(0); /* FIX: deterministic rand() so $RANDOM expansion is reproducible */
    mkdir("/tmp/zesh_fuzz_empty", 0700);
    if (chdir("/tmp/zesh_fuzz_empty") != 0) chdir("/tmp");
}

int main(int argc, char *argv[])
{
    fuzz_setup();
    FUZZ_INIT();

    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    char buf[FUZZ_BUF_SIZE];

    while (FUZZ_LOOP_FORK()) {
        size_t n = fuzz_read_one(buf, argc, argv);
        if (n == 0) continue;

        reset_globals();

        /* FIX: unique workdir per iteration; inner child's file creation stays isolated */
        char wdir[] = "/tmp/ze_XXXXXX";
        mkdtemp(wdir);

        pid_t pid = fork();
        if (pid == 0) {
            if (wdir[0]) chdir(wdir);
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) {
                dup2(dn, STDIN_FILENO);
                dup2(dn, STDOUT_FILENO);
                dup2(dn, STDERR_FILENO);
                close(dn);
            }
            alarm(2);
            char *out = expand_word(buf, 0);
            free(out);
            _exit(0);

        } else if (pid > 0) {
            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
        }
    }
    return 0;
}

