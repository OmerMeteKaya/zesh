//
// Created by mete on 24.04.2026.
//

#ifndef JOBS_H
#define JOBS_H

#include <sys/types.h>

#define MAX_JOBS 64

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} JobStatus;

typedef struct {
    int       id;
    pid_t     pgid;
    char      cmd[256];
    JobStatus status;
    int       active;
} Job;

/* jobs.c */
void  jobs_init(void);
int   job_add(pid_t pgid, const char *cmd);
void  job_set_status(pid_t pgid, JobStatus s);
void  job_remove(pid_t pgid);
void  jobs_print(void);
Job  *job_get_by_id(int id);
Job  *job_find_by_pgid(pid_t pgid);
void  jobs_disown_all(void);

/* signals.c */
void signals_init(void);


#endif //JOBS_H
