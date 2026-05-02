//
// Created by mete on 3.05.2026.
//

#ifndef SECURITY_H
#define SECURITY_H

typedef enum {
    SEC_OK = 0,
    SEC_WARN,
    SEC_BLOCK
} SecurityLevel;


SecurityLevel security_check(const char *cmdline, const char **reason);

void security_audit(const char *cmdline);

void security_init(void);

#endif