//
// Created by mete on 3.05.2026.
//

#ifndef SIGNALS_H
#define SIGNALS_H

#include <signal.h>

void signals_child(void);
void signals_init(void);
void set_fg_pid(pid_t pid);

#endif //SIGNALS_H