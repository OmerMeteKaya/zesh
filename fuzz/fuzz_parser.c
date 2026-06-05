// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// Fuzz target: lex() -> parse_list() -> cmdlist_free().
//
// The parser is the most likely place to spin on a malformed token stream,
// so we arm a 5-second SIGALRM watchdog around each parse. If it fires we
// _exit(1): to AFL++ that non-zero exit on a timeout shows up as a hang,
// and the offending input is saved under findings/parser/hangs/.
//
// NOTE: parse_list() only builds the AST; it does NOT execute anything, so
// this target never forks or runs commands. (Command substitution etc. is
// resolved later, at expansion/execution time — see fuzz_expand.c.)

#include <signal.h>
#include <unistd.h>
#include "fuzz_common.h"
#include "../include/shell.h"

static void on_alarm(int sig) {
    (void)sig;
    /* Async-signal-safe: just bail. AFL records this as a hang. */
    _exit(1);
}

int main(int argc, char *argv[]) {
    FUZZ_INIT();

    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    char buf[FUZZ_BUF_SIZE];

    while (FUZZ_LOOP_PERSIST()) {
        size_t n = fuzz_read_one(buf, argc, argv);
        if (n == 0) continue;

        int ntokens = 0;
        Token *toks = lex(buf, &ntokens);
        if (!toks) continue;

        alarm(5);                       /* hang watchdog */
        CmdList *list = parse_list(toks, ntokens);
        alarm(0);

        if (list) cmdlist_free(list);
        func_free_all();
        tokens_free(toks, ntokens);
    }
    return 0;
}
