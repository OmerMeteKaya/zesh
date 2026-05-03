//
// Created by mete on 23.04.2026.
//

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include "../include/jobs.h"
#include "../include/shell.h"

extern void set_fg_pid(pid_t pid);
extern int  last_exit_status;

int execute_list(CmdList *list) {
    if (!list || list->count == 0) return 0;

    int last_status = 0;

    for (int i = 0; i < list->count; i++) {
        CmdNode *node = &list->nodes[i];
        if (!node->pipeline) continue;

        /* Decide whether to run this pipeline based on previous status */
        /* First node always runs */
        if (i > 0) {
            ListOp prev_op = list->nodes[i-1].op;
            if (prev_op == OP_AND && last_status != 0) continue; /* skip */
            if (prev_op == OP_OR  && last_status == 0) continue; /* skip */
            /* OP_SEMI and OP_NONE: always run */
        }

        /* Check if first command is builtin */
        Pipeline *p = node->pipeline;
        if (p->ncommands > 0 &&
            p->commands[0].argc > 0 &&
            is_builtin(p->commands[0].argv[0])) {
            last_status = run_builtin(&p->commands[0]);
        } else {
            last_status = execute(p);
        }

        /* Update global last_exit_status */
        extern int last_exit_status;
        last_exit_status = last_status;
    }

    return last_status;
}

int execute(Pipeline *p) {
    if (!p || p->ncommands == 0) {
        return -1;
    }

    // Special case: no commands (should not happen if parser is correct)
    if (p->ncommands == 0) {
        return 0;
    }

    // Single command case
    if (p->ncommands == 1) {
        Command *cmd = &p->commands[0];
        
        /* --- PARENT: setup heredoc pipe before fork --- */
        int heredoc_pipe[2] = {-1, -1};
        if (cmd->heredoc_content) {
            if (pipe(heredoc_pipe) < 0) {
                perror("pipe");
                return -1;
            }
            
            /* expand variables in heredoc content */
            const char *content_to_write = cmd->heredoc_content;
            char *expanded_content = NULL;
            if (cmd->heredoc_expand) {
                extern char *expand_word(const char *word, int last_exit);
                extern int last_exit_status;
                
                expanded_content = expand_word(cmd->heredoc_content,
                                                last_exit_status);
                if (expanded_content) content_to_write = expanded_content;
            }
            
            size_t hlen = strlen(content_to_write);
            size_t written = 0;
            while (written < hlen) {
                ssize_t w = write(heredoc_pipe[1],
                                  content_to_write + written,
                                  hlen - written);
                if (w <= 0) break;
                written += w;
            }
            close(heredoc_pipe[1]);
            free(expanded_content);
        }

        pid_t pid = fork();
        if (pid == 0) {
            signals_child();

            /* heredoc stdin */
            if (heredoc_pipe[0] >= 0) {
                dup2(heredoc_pipe[0], STDIN_FILENO);
                close(heredoc_pipe[0]);
            } else if (cmd->infile) {
                int fd = open(cmd->infile, O_RDONLY);
                if (fd < 0) {
                    perror(cmd->infile);
                    _exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            /* outfile handling — existing code */
            if (cmd->outfile) {
                int flags = O_WRONLY|O_CREAT|(cmd->append?O_APPEND:O_TRUNC);
                int fd = open(cmd->outfile, flags, 0644);
                if (fd < 0) {
                    perror(cmd->outfile);
                    _exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            // Execute the command
            execvp(cmd->argv[0], cmd->argv);
            perror(cmd->argv[0]);
            _exit(127);
        } else if (pid > 0) {
            if (heredoc_pipe[0] >= 0) close(heredoc_pipe[0]);
            setpgid(pid, pid);
            
            if (p->background) {
                // Build command string for job
                char cmd_str[256] = {0};
                for (int i = 0; i < cmd->argc && i < 10; i++) {  // Limit to prevent overflow
                    if (i > 0) strcat(cmd_str, " ");
                    strcat(cmd_str, cmd->argv[i]);
                }
                
                int job_id = job_add(pid, cmd_str);
                printf("[%d] %d\n", job_id, pid);
                return 0;
            } else {
                // Give terminal control to child process group
                tcsetpgrp(STDIN_FILENO, pid);
                set_fg_pid(pid);
                
                int status;
                pid_t result = waitpid(pid, &status, WUNTRACED);
                
                set_fg_pid(0);
                // Take terminal control back
                tcsetpgrp(STDIN_FILENO, getpgrp());
                
                // Handle stopped process
                if (result > 0 && WIFSTOPPED(status)) {
                    // Build command string for job
                    char cmd_str[256] = {0};
                    for (int i = 0; i < cmd->argc && i < 10; i++) {  // Limit to prevent overflow
                        if (i > 0) strcat(cmd_str, " ");
                        strcat(cmd_str, cmd->argv[i]);
                    }
                    
                    int job_id = job_add(pid, cmd_str);
                    job_set_status(pid, JOB_STOPPED);
                    printf("[%d]+  Stopped\t\t%s\n", job_id, cmd_str);
                    return 0;
                }
                
                if (WIFEXITED(status)) {
                    return WEXITSTATUS(status);
                } else {
                    return -1;
                }
            }
        } else {
            // Fork failed
            perror("fork");
            return -1;
        }
    } else {
        // Multiple commands with pipes
        int **pipes = malloc((p->ncommands - 1) * sizeof(int*));
        if (!pipes) {
            perror("malloc");
            return -1;
        }
        
        for (int i = 0; i < p->ncommands - 1; i++) {
            pipes[i] = malloc(2 * sizeof(int));
            if (!pipes[i]) {
                perror("malloc");
                // Free previously allocated pipes
                for (int j = 0; j < i; j++) {
                    free(pipes[j]);
                }
                free(pipes);
                return -1;
            }
            if (pipe(pipes[i]) < 0) {
                perror("pipe");
                // Free previously allocated pipes
                for (int j = 0; j <= i; j++) {
                    free(pipes[j]);
                }
                free(pipes);
                return -1;
            }
        }
        
        pid_t *pids = malloc(p->ncommands * sizeof(pid_t));
        if (!pids) {
            perror("malloc");
            // Free pipes
            for (int i = 0; i < p->ncommands - 1; i++) {
                free(pipes[i]);
            }
            free(pipes);
            return -1;
        }
        
        pid_t pgid = 0;
        
        // Fork children for each command
        for (int i = 0; i < p->ncommands; i++) {
            Command *cmd = &p->commands[i];
            pid_t pid = fork();
            
            if (pid == 0) {
                // Child process
                signals_child();
                
                // Set process group ID
                if (i == 0) {
                    pgid = getpid();
                }
                setpgid(0, pgid);
                
                // Handle input redirection for first command
                if (i == 0 && cmd->infile) {
                    int fd = open(cmd->infile, O_RDONLY);
                    if (fd < 0) {
                        perror(cmd->infile);
                        _exit(1);
                    }
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                }
                // Handle output redirection for last command
                else if (i == p->ncommands - 1 && cmd->outfile) {
                    int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
                    int fd = open(cmd->outfile, flags, 0644);
                    if (fd < 0) {
                        perror(cmd->outfile);
                        _exit(1);
                    }
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }
                
                // Connect pipes
                if (i > 0) {
                    // Connect previous pipe's read end to stdin
                    dup2(pipes[i-1][0], STDIN_FILENO);
                }
                if (i < p->ncommands - 1) {
                    // Connect current pipe's write end to stdout
                    dup2(pipes[i][1], STDOUT_FILENO);
                }
                
                // Close all pipe file descriptors
                for (int j = 0; j < p->ncommands - 1; j++) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                
                // Execute the command
                execvp(cmd->argv[0], cmd->argv);
                perror(cmd->argv[0]);
                _exit(127);
            } else if (pid > 0) {
                pids[i] = pid;
                
                // Set process group ID in parent (race condition fix)
                if (i == 0) {
                    pgid = pid;
                }
                setpgid(pid, pgid);
            } else {
                perror("fork");
                // Close all pipe file descriptors
                for (int j = 0; j < p->ncommands - 1; j++) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                // Free memory
                for (int j = 0; j < p->ncommands - 1; j++) {
                    free(pipes[j]);
                }
                free(pipes);
                free(pids);
                return -1;
            }
        }
        
        // Parent process - close all pipe file descriptors
        for (int i = 0; i < p->ncommands - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
            free(pipes[i]);
        }
        free(pipes);
        
        // Handle background pipeline
        if (p->background) {
            // Build command string for job
            char cmd_str[256] = {0};
            for (int i = 0; i < p->commands[0].argc && i < 10; i++) {  // Limit to prevent overflow
                if (i > 0) strcat(cmd_str, " ");
                strcat(cmd_str, p->commands[0].argv[i]);
            }
            
            int job_id = job_add(pgid, cmd_str);
            printf("[%d] %d\n", job_id, pgid);
            free(pids);
            return 0;
        }
        
        // Foreground pipeline
        // Give terminal control to child process group
        tcsetpgrp(STDIN_FILENO, pgid);
        set_fg_pid(pgid);
        
        // Wait for children
        int status = 0;
        int last_status = 0;
        int done = 0;
        int stopped = 0;

        while (done + stopped < p->ncommands) {
            pid_t w = waitpid(-pgid, &status, WUNTRACED);
            if (w <= 0) break;
            if (WIFSTOPPED(status)) {
                stopped++;
                /* sigchld_handler already updated job table */
            } else {
                done++;
                if (WIFEXITED(status)) last_status = WEXITSTATUS(status);
            }
        }

        set_fg_pid(0);
        tcsetpgrp(STDIN_FILENO, getpgrp());

        if (stopped > 0 && done < p->ncommands) {
            /* at least one command stopped — add whole pipeline as stopped job */
            char cmd_str[256] = {0};
            for (int i = 0; i < p->commands[0].argc && i < 10; i++) {
                if (i > 0) strncat(cmd_str, " ", sizeof(cmd_str)-strlen(cmd_str)-1);
                strncat(cmd_str, p->commands[0].argv[i], sizeof(cmd_str)-strlen(cmd_str)-1);
            }
            int job_id = job_add(pgid, cmd_str);
            job_set_status(pgid, JOB_STOPPED);
            printf("[%d]+  Stopped\t\t%s\n", job_id, cmd_str);
        }

        free(pids);
        return last_status;
    }
}
