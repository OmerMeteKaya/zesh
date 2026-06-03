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

/* One-time hardening before the fork server starts. */
static void fuzz_setup(void) {
    /* Make PATH lookups fail so `$(ls)` etc. can't run system binaries. */
    setenv("PATH", "", 1);
    /* Contain any file creation to a scratch dir. */
    if (chdir("/tmp") != 0) { /* best-effort */ }
}

int main(int argc, char *argv[]) {
    fuzz_setup();
    FUZZ_INIT();

    char buf[FUZZ_BUF_SIZE];

    while (FUZZ_LOOP()) {
        size_t n = fuzz_read_one(buf, argc, argv);
        if (n == 0) continue;

        char *out = expand_word(buf, 0);
        free(out);
    }
    return 0;
}
