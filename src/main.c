//
// Created by mete on 23.04.2026.
//

extern int last_exit_status;

#include <time.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/shell.h"
#include "../include/input.h"
#include "../include/alias.h"
#include "../include/rc.h"
#include "../include/plugin.h"
#include "../include/config.h"
#include "../include/security.h"

void signals_init(void);
void jobs_init(void);



int main() {
    // Initialize signals and jobs
    signals_init();
    jobs_init();
    char history_path[512];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(history_path, sizeof(history_path), "%s/.mysh_history", home);
    } else {
        snprintf(history_path, sizeof(history_path), ".mysh_history");
    }
    history_init(history_path);
    // Home
    char mysh_dir[512];
    if (home)
        snprintf(mysh_dir, sizeof(mysh_dir), "%s/.mysh", home);
    mkdir(mysh_dir, 0755);

    // Alias
    alias_init();

    // Config
    char config_path[512];
    if (home)
        snprintf(config_path, sizeof(config_path),
                 "%s/.mysh/config", home);
    else
        snprintf(config_path, sizeof(config_path), ".mysh/config");
    config_load(config_path);

    // Plugin
    char plugin_dir[512];
    if (home)
        snprintf(plugin_dir, sizeof(plugin_dir), "%s/.mysh/plugins", home);
    else
        snprintf(plugin_dir, sizeof(plugin_dir), ".mysh/plugins");
    plugins_init(plugin_dir);
    char rc_path[512];
    if (home) {
        snprintf(rc_path, sizeof(rc_path), "%s/.myshrc", home);
    } else {
        snprintf(rc_path, sizeof(rc_path), ".myshrc");
    }
    rc_load(rc_path);

    // Take shell process group ownership
    pid_t shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    tcsetpgrp(STDIN_FILENO, shell_pgid);
    
    
    while (1) {
        char prompt[512];
        char cwd[256];
        const char *home = getenv("HOME");

        if (getcwd(cwd, sizeof(cwd))) {
            char display[256];
            if (home && strcmp(cwd, home) == 0) {
                strncpy(display, "~", sizeof(display));
            } else {
                char *last = strrchr(cwd, '/');
                if (last && *(last+1))
                    strncpy(display, last+1, sizeof(display));
                else
                    strncpy(display, cwd, sizeof(display));
            }

            /* plugin hook — git branch vb. */
            char *left = hook_prompt_left();
            if (left) {
                /* git varsa: ➜  HH:MM user dir (branch) */
                char time_part[32] = "";
                char user_part[64] = "";
                
                /* saat */
                if (g_config.prompt_show_time) {
                    time_t t = time(NULL);
                    struct tm *tm = localtime(&t);
                    char timebuf[8];
                    strftime(timebuf, sizeof(timebuf), "%H:%M", tm);
                    snprintf(time_part, sizeof(time_part), "\033[0;37m%s\033[0m ", timebuf);
                }
                
                /* kullanıcı */
                if (g_config.prompt_show_user) {
                    const char *user = getenv("USER");
                    if (user) {
                        snprintf(user_part, sizeof(user_part), "\033[1;36m%s\033[0m ", user);
                    }
                }

                snprintf(prompt, sizeof(prompt),
                    "\033[1;32m➜\033[0m  "        /* yeşil ok */
                    "%s"                           /* saat */
                    "%s"                           /* kullanıcı */
                    "\033[1;34m%s\033[0m "         /* mavi dizin */
                    "%s"                           /* git (zaten renkli) */
                    "\033[0m> ",                   /* reset + > */
                    time_part,
                    user_part,
                    display,
                    left);
                free(left);
            } else {
                char time_part[32] = "";
                char user_part[64] = "";
                
                if (g_config.prompt_show_time) {
                    time_t t = time(NULL);
                    struct tm *tm = localtime(&t);
                    char timebuf[8];
                    strftime(timebuf, sizeof(timebuf), "%H:%M", tm);
                    snprintf(time_part, sizeof(time_part), "\033[0;37m%s\033[0m ", timebuf);
                }
                
                if (g_config.prompt_show_user) {
                    const char *user = getenv("USER");
                    if (user) {
                        snprintf(user_part, sizeof(user_part), "\033[1;36m%s\033[0m ", user);
                    }
                }

                snprintf(prompt, sizeof(prompt),
                    "\033[1;32m➜\033[0m  "
                    "%s"
                    "%s"
                    "\033[1;34m%s\033[0m> ",
                    time_part,
                    user_part,
                    display);
            }
        } else {
            snprintf(prompt, sizeof(prompt), "➜ mysh> ");
        }
        char *input = read_line(prompt);
        if (!input) {
            printf("\n");
            break;
        }
        /* strip trailing newline — artık gerekmiyor ama zarar vermez */
        input[strcspn(input, "\n")] = '\0';
        if (strlen(input) == 0) { free(input); continue; }
        
        // Strip trailing newline
        input[strcspn(input, "\n")] = '\0';
        
        // Skip empty lines
        if (strlen(input) == 0) {
            continue;
        }
        int ntokens;
        Token *tokens = lex(input, &ntokens);

        int is_assignment = 0;
        if (ntokens == 2 &&              /* sadece bir token + EOF */
            tokens[0].type == TOK_WORD &&
            tokens[0].value) {
            char *eq = strchr(tokens[0].value, '=');
            if (eq && eq != tokens[0].value) {  /* = var ve başta değil */
                /* KEY=VALUE — local variable set */
                *eq = '\0';
                char *key = tokens[0].value;
                char *val = eq + 1;
                extern void local_var_set(const char *name, const char *value);
                local_var_set(key, val);
                *eq = '=';  /* restore */
                is_assignment = 1;
            }
            }

        // Lexical analysis


        if (!tokens) {
            fprintf(stderr, "Error during lexical analysis\n");
            continue;
        }

        tokens = glob_expand_tokens(tokens, &ntokens, last_exit_status);


        if (ntokens > 0 && tokens[0].type == TOK_WORD && tokens[0].value) {
            char *expanded = alias_expand(tokens[0].value);
            if (expanded) {

                char combined[4096];
                snprintf(combined, sizeof(combined), "%s", expanded);
                for (int i = 1; i < ntokens - 1; i++) { /* -1 for EOF */
                    if (tokens[i].value) {
                        strncat(combined, " ", sizeof(combined)-strlen(combined)-1);
                        strncat(combined, tokens[i].value, sizeof(combined)-strlen(combined)-1);
                    }
                }
                tokens_free(tokens, ntokens);
                tokens = lex(combined, &ntokens);
                tokens = brace_expand_tokens(tokens, &ntokens);
                if (!tokens) continue;
                tokens = glob_expand_tokens(tokens, &ntokens, last_exit_status);
                if (!tokens) continue;
            }
        }

        // Execution
        if (!is_assignment) {

            const char *sec_reason = NULL;
            SecurityLevel sec_level = security_check(input, &sec_reason);

            int should_run = 1;

            if (sec_level == SEC_BLOCK) {
                fprintf(stderr,
                    "\033[1;31m⛔ Blocked:\033[0m %s\n", sec_reason);
                should_run = 0;
            } else if (sec_level == SEC_WARN) {
                fprintf(stderr,
                    "\033[1;33m⚠  Unsafe Command:\033[0m %s\n", sec_reason);
                fprintf(stderr,
                    "\033[1;33m   Do you want to continue?[y/N]\033[0m ");
                fflush(stderr);

                
                char answer[4] = {0};
                if (fgets(answer, sizeof(answer), stdin)) {
                    should_run = (answer[0] == 'y' || answer[0] == 'Y');
                } else {
                    should_run = 0;
                }
            }

            /* audit log */
            security_audit(input);

            if (should_run) {
                CmdList *list = parse_list(tokens, ntokens);
                if (list) {
                    execute_list(list);
                    cmdlist_free(list);
                }
            }
        }
        // Cleanup
        tokens_free(tokens, ntokens);
        free(input);
    }
        history_close();
        alias_free();
        plugins_unload();
        return 0;
    }

