// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// Shared scaffolding for the Zesh fuzz targets (lexer / parser / expand).
//
// Each target reads one test case (from argv[1] if given, otherwise from
// stdin) into a fixed buffer and hands it to the function under test.
//
//   * Under AFL++ instrumentation the work runs inside __AFL_LOOP for
//     "persistent mode" (orders of magnitude faster than fork/exec).
//   * Built without AFL (plain cc/clang, e.g. the *_asan targets) the
//     same source still compiles and runs the body exactly once over a
//     single input, which is what triage_crashes.sh needs to reproduce
//     a crash under a sanitizer.

#ifndef ZESH_FUZZ_COMMON_H
#define ZESH_FUZZ_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FUZZ_BUF_SIZE
#define FUZZ_BUF_SIZE 16384
#endif

/* AFL++ deferred-init: lets the fork server start after one-time setup. */
#ifdef __AFL_HAVE_MANUAL_CONTROL
#define FUZZ_INIT() __AFL_INIT()
#else
#define FUZZ_INIT() ((void)0)
#endif

/*
 * Loop condition for the persistent-mode driver.
 *   - With AFL++: __AFL_LOOP(N) returns non-zero up to N times, snapshotting
 *     state between iterations, then 0 (process is recycled).
 *   - Without AFL++: run the body exactly once (single-shot reproduction).
 */
#ifdef __AFL_LOOP
#define FUZZ_LOOP_PERSIST() __AFL_LOOP(1000)
#else
#define FUZZ_LOOP_PERSIST() (fuzz_once_only())
#endif

/* Fork targets: persistent mode OFF — always single-shot inside AFL fork-server */
#define FUZZ_LOOP_FORK() (fuzz_once_only())

static inline int fuzz_once_only(void) {
    static int done = 0;
    if (done) return 0;
    done = 1;
    return 1;
}

/* Read up to FUZZ_BUF_SIZE-1 bytes of one test case into `buf`.
 * Returns the byte count (0 on empty/EOF). Always NUL-terminates `buf`. */
static inline size_t fuzz_read_one(char *buf, int argc, char **argv) {
    size_t n = 0;
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (f) {
            n = fread(buf, 1, FUZZ_BUF_SIZE - 1, f);
            fclose(f);
        }
    } else {
        n = fread(buf, 1, FUZZ_BUF_SIZE - 1, stdin);
    }
    buf[n] = '\0';
    return n;
}

#endif /* ZESH_FUZZ_COMMON_H */
