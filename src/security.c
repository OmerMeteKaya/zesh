//
// Created by mete on 23.04.2026.
//

#include "../include/security.h"
#include "../include/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Danger rule table */

typedef struct {
    const char *pattern;   /* command or pattern to match */
    int         is_regex;  /* 0=substring match, 1=prefix match */
    SecurityLevel level;
    const char *reason;
} DangerRule;
static const DangerRule rules[] = {
    /* Disk/filesystem destruction */
    { "rm -rf /",        0, SEC_WARN,  "Entire filesystem will be deleted!" },
    { "rm -rf /*",       0, SEC_WARN,  "Entire filesystem will be deleted!" },
    { "rm -fr /",        0, SEC_WARN,  "Entire filesystem will be deleted!" },
    { "dd if=/dev/zero", 0, SEC_WARN,  "Disk will be completely overwritten with zeros!" },
    { "dd if=/dev/urandom", 0, SEC_WARN, "Disk will be overwritten with random data!" },
    { "mkfs.",           1, SEC_WARN,  "Filesystem will be formatted!" },
    { "fdisk",           1, SEC_WARN,  "Disk partitioning tool" },
    { "parted",          1, SEC_WARN,  "Disk partitioning tool" },
    { "wipefs",          1, SEC_WARN,  "Disk signatures will be erased!" },
    { "> /dev/sd",       0, SEC_WARN,  "Direct write to disk device!" },

    /* System-wide changes */
    { "chmod -R 777 /",  0, SEC_WARN,  "All system permissions will be altered!" },
    { "chown -R",        0, SEC_WARN,  "Ownership is being modified recursively!" },
    { "chmod 777",       0, SEC_WARN,  "Insecure permissions (777)!" },

    /* Fork bomb */
    { ":(){ :|:& };:",   0, SEC_BLOCK, "Fork bomb detected!" },
    { ":(){ :|: &};:",   0, SEC_BLOCK, "Fork bomb detected!" },

    /* Sensitive file operations */
    { "shred /dev/",     0, SEC_WARN,  "Device data will be permanently erased!" },
    { "rm -rf ~",        0, SEC_WARN,  "Home directory will be deleted!" },
    { "rm -rf $HOME",    0, SEC_WARN,  "Home directory will be deleted!" },

    /* Network danger */
    { "nc -e /bin/sh",   0, SEC_WARN,  "Reverse shell detected!" },
    { "nc -e /bin/bash", 0, SEC_WARN,  "Reverse shell detected!" },
    { "bash -i >& /dev/tcp", 0, SEC_WARN, "Reverse shell detected!" },

    /* History/audit tampering */
    { "rm ~/.mysh_history", 0, SEC_WARN, "Shell history will be deleted!" },
    { "> ~/.mysh_history",  0, SEC_WARN, "Shell history will be cleared!" },
    { NULL, 0, SEC_OK, NULL }
};

SecurityLevel security_check(const char *cmdline, const char **reason) {
    if (!cmdline || !g_config.security_warn) return SEC_OK;

    SecurityLevel highest = SEC_OK;
    const char *highest_reason = NULL;

    for (int i = 0; rules[i].pattern != NULL; i++) {
        int match = 0;
        if (rules[i].is_regex == 1) {
            /* prefix match — cmdline starts with pattern
               (after stripping leading spaces) */
            const char *p = cmdline;
            while (*p == ' ') p++;
            match = (strncmp(p, rules[i].pattern,
                             strlen(rules[i].pattern)) == 0);
        } else {
            /* substring match */
            match = (strstr(cmdline, rules[i].pattern) != NULL);
        }

        if (match) {
            if (rules[i].level > highest) {
                highest = rules[i].level;
                highest_reason = rules[i].reason;
            }
        }
    }

    if (reason) *reason = highest_reason;
    return highest;
}

void security_audit(const char *cmdline) {
    if (!g_config.security_audit || !cmdline) return;

    /* expand ~ in audit log path */
    char log_path[512];
    const char *home = getenv("HOME");
    if (home && strncmp(g_config.security_audit_log, "~/", 2) == 0) {
        snprintf(log_path, sizeof(log_path), "%s/%s",
                 home, g_config.security_audit_log + 2);
    } else {
        strncpy(log_path, g_config.security_audit_log,
                sizeof(log_path)-1);
    }

    FILE *f = fopen(log_path, "a");
    if (!f) return;

    /* timestamp */
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(f, "[%s] %s\n", timebuf, cmdline);
    fclose(f);
}

void security_init(void) {
    /* nothing for now — config already loaded */
}
