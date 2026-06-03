// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// Fuzz target: full shell pipeline — lex() -> glob_expand_tokens() ->
// parse_list() -> execute_list().
//
// Execution is sandboxed: each input runs in a forked child with
// stdin/stdout/stderr redirected to /dev/null and a 2-second SIGALRM
// watchdog. The parent waits for the child; if it hangs AFL++ records
// the input under findings/script/hangs/.
//
// Global shell state is reset between fuzz iterations so one malformed
// input cannot poison the next.

#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "fuzz_common.h"
#include "../include/shell.h"

extern void tokens_free(Token *toks, int n);
extern Token *glob_expand_tokens(Token *toks, int *ntokens, int last_exit);

extern int g_opt_errexit;
extern int g_opt_xtrace;
extern int g_opt_pipefail;
extern int g_returning;
extern int g_return_value;
extern volatile int g_interrupt_loop;
extern int g_expand_error;

static void on_alarm(int sig) {
    (void)sig;
    _exit(1);
}

static void reset_globals(void) {
    g_opt_errexit  = 0;
    g_opt_xtrace   = 0;
    g_opt_pipefail = 0;
    g_returning    = 0;
    g_return_value = 0;
    g_interrupt_loop = 0;
    g_expand_error   = 0;
}

int main(int argc, char *argv[]) {
    FUZZ_INIT();

    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    char buf[FUZZ_BUF_SIZE];

    while (FUZZ_LOOP()) {
        size_t n = fuzz_read_one(buf, argc, argv);
        if (n == 0) continue;

        alarm(2);

        int ntokens = 0;
        Token *toks = lex(buf, &ntokens);
        if (!toks) { alarm(0); continue; }

        toks = glob_expand_tokens(toks, &ntokens, 0);
        if (!toks) { alarm(0); continue; }

        CmdList *list = parse_list(toks, ntokens);
        tokens_free(toks, ntokens);

        if (list) {
            pid_t pid = fork();
            if (pid == 0) {
                /* child: suppress all I/O and execute with tight timeout */
                int dn = open("/dev/null", O_RDWR);
                if (dn >= 0) {
                    dup2(dn, STDIN_FILENO);
                    dup2(dn, STDOUT_FILENO);
                    dup2(dn, STDERR_FILENO);
                    close(dn);
                }
                alarm(1);
                execute_list(list);
                cmdlist_free(list);
                _exit(0);
            } else if (pid > 0) {
                int status;
                waitpid(pid, &status, 0);
                cmdlist_free(list);
            }
        }

        alarm(0);
        reset_globals();
    }
    return 0;
}
