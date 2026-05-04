//
// Created by mete on 23.04.2026.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <termios.h>
#include "../include/jobs.h"
#include "../include/alias.h"
#include "../include/rc.h"
#include "../include/shell.h"

static void restore_terminal(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag |= (ICANON | ECHO | ISIG);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    /* move to new line so prompt is clean */
    write(STDOUT_FILENO, "\r\n", 2);
}

static void echo_print_escaped(const char *s) {
    while (*s) {
        if (*s == '\\' && *(s+1)) {
            s++;
            switch (*s) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case '\\': putchar('\\'); break;
                case 'a': putchar('\a'); break;
                case 'b': putchar('\b'); break;
                case '0': putchar('\0'); break;
                default: putchar('\\'); putchar(*s); break;
            }
            s++;
        } else {
            putchar(*s++);
        }
    }
}

int is_builtin(const char *cmd) {
    if (!cmd) return 0;
    
    return (strcmp(cmd, "cd") == 0) || 
           (strcmp(cmd, "exit") == 0) || 
           (strcmp(cmd, "export") == 0) || 
           (strcmp(cmd, "unset") == 0) || 
           (strcmp(cmd, "pwd") == 0) || 
           (strcmp(cmd, "echo") == 0) ||
           (strcmp(cmd, "jobs") == 0) ||
           (strcmp(cmd, "fg") == 0) ||
           (strcmp(cmd, "bg") == 0) ||
           (strcmp(cmd, "alias") == 0) ||
           (strcmp(cmd, "unalias") == 0) ||
           (strcmp(cmd, "source") == 0) ||
           (strcmp(cmd, ".") == 0);
}

int run_builtin(Command *cmd) {
    if (!cmd || !cmd->argv || cmd->argc == 0) {
        return 1;
    }
    
    const char *builtin_cmd = cmd->argv[0];
    
    if (strcmp(builtin_cmd, "cd") == 0) {
        const char *path = getenv("HOME");
        if (cmd->argc > 1) {
            path = cmd->argv[1];
        }
        
        if (chdir(path) != 0) {
            perror("cd");
            return 1;
        }
        return 0;
    }
    
    if (strcmp(builtin_cmd, "exit") == 0) {
        restore_terminal();
        exit(cmd->argc > 1 ? atoi(cmd->argv[1]) : 0);
    }
    
    if (strcmp(builtin_cmd, "export") == 0) {
        if (cmd->argc <= 1) {
            return 1;
        }
        
        char *eq = strchr(cmd->argv[1], '=');
        if (!eq) {
            return 1;
        }
        
        *eq = '\0';
        const char *key = cmd->argv[1];
        const char *value = eq + 1;
        
        if (setenv(key, value, 1) != 0) {
            *eq = '='; // Restore the string
            return 1;
        }
        *eq = '='; // Restore the string
        return 0;
    }
    
    if (strcmp(builtin_cmd, "unset") == 0) {
        if (cmd->argc <= 1) {
            return 1;
        }
        
        if (unsetenv(cmd->argv[1]) != 0) {
            return 1;
        }
        return 0;
    }
    
    if (strcmp(builtin_cmd, "pwd") == 0) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
            return 0;
        } else {
            perror("pwd");
            return 1;
        }
    }
    
    if (strcmp(builtin_cmd, "echo") == 0) {
        int escape = 0;
        int start_arg = 1;
        if (cmd->argc >= 2 && strcmp(cmd->argv[1], "-e") == 0) {
            escape = 1;
            start_arg = 2;
        }
        int no_newline = 0;
        if (cmd->argc >= 2 && strcmp(cmd->argv[start_arg-1+!escape], "-n") == 0) {
            no_newline = 1;
            start_arg++;
        }
        
        for (int i = start_arg; i < cmd->argc; i++) {
            if (i > start_arg) {
                putchar(' ');
            }
            if (escape) {
                echo_print_escaped(cmd->argv[i]);
            } else {
                fputs(cmd->argv[i], stdout);
            }
        }
        
        if (!no_newline) {
            putchar('\n');
        }
        
        return 0;
    }
    
    if (strcmp(builtin_cmd, "jobs") == 0) {
        jobs_print();
        return 0;
    }
    
    if (strcmp(builtin_cmd, "fg") == 0) {
        int job_id = -1;
        
        if (cmd->argc > 1) {
            // Parse job ID if provided
            if (cmd->argv[1][0] == '%') {
                job_id = atoi(cmd->argv[1] + 1);
            } else {
                fprintf(stderr, "fg: invalid job specification\n");
                return 1;
            }
        }
        
        Job *job;
        if (job_id == -1) {
            // Find the last active job
            job = NULL;
            for (int i = MAX_JOBS - 1; i >= 0; i--) {
                job = job_get_by_id(i + 1); // Assuming IDs start from 1
                if (job) {
                    break;
                }
            }
            if (!job) {
                fprintf(stderr, "fg: no current job\n");
                return 1;
            }
        } else {
            job = job_get_by_id(job_id);
            if (!job) {
                fprintf(stderr, "fg: no such job\n");
                return 1;
            }
        }
        
        pid_t pgid = job->pgid;
        
        // Give the job's process group control of the terminal
        if (tcsetpgrp(STDIN_FILENO, pgid) == -1) {
            perror("tcsetpgrp");
            return 1;
        }
        
        // Send SIGCONT to resume the job if it was stopped
        if (kill(-pgid, SIGCONT) == -1) {
            perror("kill");
            return 1;
        }
        
        // Wait for the job to complete or stop
        int status;
        pid_t result;
        do {
            result = waitpid(-pgid, &status, WUNTRACED);
        } while (result > 0 && !WIFEXITED(status) && !WIFSIGNALED(status) && !WIFSTOPPED(status));
        
        // Take back terminal control
        if (tcsetpgrp(STDIN_FILENO, getpgrp()) == -1) {
            perror("tcsetpgrp");
        }
        
        // Update job status
        if (result > 0) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                job_remove(pgid);
            } else if (WIFSTOPPED(status)) {
                job_set_status(pgid, JOB_STOPPED);
            }
        }
        
        return 0;
    }
    
    if (strcmp(builtin_cmd, "bg") == 0) {
        int job_id = -1;
        pid_t pgid;
        
        if (cmd->argc > 1) {
            // Parse job ID if provided
            if (cmd->argv[1][0] == '%') {
                job_id = atoi(cmd->argv[1] + 1);
            } else {
                job_id = atoi(cmd->argv[1]);
            }
        }
        
        Job *job;
        if (job_id == -1) {
            // Find the last active job
            job = NULL;
            for (int i = MAX_JOBS - 1; i >= 0; i--) {
                job = job_get_by_id(i + 1); // Assuming IDs start from 1
                if (job) {
                    break;
                }
            }
            if (!job) {
                fprintf(stderr, "bg: no current job\n");
                return 1;
            }
        } else {
            job = job_get_by_id(job_id);
            if (!job) {
                fprintf(stderr, "bg: no such job\n");
                return 1;
            }
        }
        
        pgid = job->pgid;
        
        // Send SIGCONT to resume the job
        if (kill(-pgid, SIGCONT) == -1) {
            perror("kill");
            return 1;
        }
        
        // Update job status
        job_set_status(pgid, JOB_RUNNING);
        
        // Print job status
        printf("[%d]+ %s &\n", job->id, job->cmd);
        
        return 0;
    }
    
    if (strcmp(builtin_cmd, "alias") == 0) {
        if (cmd->argc == 1) {
            alias_list();
            return 0;
        }

        char combined[4096] = {0};
        for (int i = 1; i < cmd->argc; i++) {
            if (i > 1) strncat(combined, " ", sizeof(combined)-strlen(combined)-1);
            strncat(combined, cmd->argv[i], sizeof(combined)-strlen(combined)-1);
        }

        char *eq = strchr(combined, '=');
        if (!eq) return 1;

        *eq = '\0';
        char *name = combined;
        char *value = eq + 1;

        while (*value == ' ') value++;

        while (*value == '\'' || *value == '"') {
            char quote = *value;
            int vlen = strlen(value);
            if (vlen >= 2 && value[vlen-1] == quote) {
                value++;
                value[strlen(value)-1] = '\0';
            } else {
                break;
            }
        }

        while ((*value == '\'' || *value == '"')) {
            char quote = *value;
            int vlen = strlen(value);
            if (vlen >= 2 && value[vlen-1] == quote) {
                value++;
                value[strlen(value)-1] = '\0';
            } else {
                break;
            }
        }
        alias_add(name, value);
        return 0;
    }
    
    if (strcmp(builtin_cmd, "source") == 0 ||
        strcmp(builtin_cmd, ".") == 0) {
        if (cmd->argc < 2) {
            fprintf(stderr, "%s: filename required\n", builtin_cmd);
            return 1;
        }
        rc_load(cmd->argv[1]);
        return 0;
    }
    
    return 1; // Not a builtin command
}
