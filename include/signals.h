// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya

#ifndef SIGNALS_H
#define SIGNALS_H

#include <signal.h>

void signals_child(void);
void signals_init(void);
void set_fg_pid(pid_t pid);

/* trap state */
#define TRAP_NSIG 32
extern char *g_trap_actions[TRAP_NSIG];
extern char *g_trap_exit;
void trap_run_exit(int code);

#endif //SIGNALS_H