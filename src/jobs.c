//
// Created by mete on 23.04.2026.
//

#include <string.h>
#include <stdio.h>
#include "../include/jobs.h"

static Job table[MAX_JOBS];
static int next_id = 1;

void jobs_init(void) {
    memset(table, 0, sizeof(table));
    next_id = 1;
}

int job_add(pid_t pgid, const char *cmd) {
    // Find first inactive slot
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!table[i].active) {
            table[i].id = next_id++;
            table[i].pgid = pgid;
            strncpy(table[i].cmd, cmd, sizeof(table[i].cmd) - 1);
            table[i].cmd[sizeof(table[i].cmd) - 1] = '\0';
            table[i].status = JOB_RUNNING;
            table[i].active = 1;
            return table[i].id;
        }
    }
    return -1; // Table full
}

void job_set_status(pid_t pgid, JobStatus s) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (table[i].active && table[i].pgid == pgid) {
            table[i].status = s;
            return;
        }
    }
}

void job_remove(pid_t pgid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (table[i].active && table[i].pgid == pgid) {
            table[i].active = 0;
            return;
        }
    }
}

void jobs_print(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (table[i].active) {
            const char *status_str;
            switch (table[i].status) {
                case JOB_RUNNING:
                    status_str = "Running";
                    break;
                case JOB_STOPPED:
                    status_str = "Stopped";
                    break;
                case JOB_DONE:
                    status_str = "Done";
                    break;
                default:
                    status_str = "Unknown";
            }
            
            // Determine if this is the current job (+) or previous job (-)
            char indicator = ' ';
            // Find the last active job for +
            int last_active = -1;
            for (int j = MAX_JOBS - 1; j >= 0; j--) {
                if (table[j].active) {
                    last_active = j;
                    break;
                }
            }
            
            if (last_active == i) {
                indicator = '+';
            } else {
                // Check if this is the second to last active job
                int second_last = -1;
                for (int j = last_active - 1; j >= 0; j--) {
                    if (table[j].active) {
                        second_last = j;
                        break;
                    }
                }
                if (second_last == i) {
                    indicator = '-';
                }
            }
            
            printf("[%d]%c %s\t%s\n", table[i].id, indicator, status_str, table[i].cmd);
        }
    }
}

Job *job_get_by_id(int id) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (table[i].active && table[i].id == id) {
            return &table[i];
        }
    }
    return NULL;
}

Job *job_find_by_pgid(pid_t pgid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (table[i].active && table[i].pgid == pgid) {
            return &table[i];
        }
    }
    return NULL;
}

void jobs_disown_all(void) {
    for (int i = 0; i < MAX_JOBS; i++)
        table[i].active = 0;
}
