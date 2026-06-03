// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// Stubs for symbols that live in main.c, which the fuzz targets deliberately
// exclude (main.c owns the real `main`, the REPL and the script loader).
//
// signals.c calls run_script_line() from its trap handlers. The fuzz targets
// never install traps, and stubbing it to a no-op also guarantees that fuzzed
// input can never get a trap action executed — a free safety win.

#include "../include/shell.h"

int run_script_line(const char *input) {
    (void)input;
    return 0;
}
