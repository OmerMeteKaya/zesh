// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// Fuzz target: the lexer.  Feeds arbitrary bytes to lex() and frees the
// resulting token stream. Catches buffer overruns, OOB reads/writes and
// use-after-free inside tokenization (run the *_asan build to see them).

#include "fuzz_common.h"
#include "../include/shell.h"

int main(int argc, char *argv[]) {
    FUZZ_INIT();
    char buf[FUZZ_BUF_SIZE];

    while (FUZZ_LOOP_PERSIST()) {
        size_t n = fuzz_read_one(buf, argc, argv);
        if (n == 0) continue;

        int ntokens = 0;
        Token *toks = lex(buf, &ntokens);
        if (toks) tokens_free(toks, ntokens);
    }
    return 0;
}
