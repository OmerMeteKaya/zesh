//
// Created by mete on 23.04.2026.
//

#include <ctype.h>
extern int last_exit_status;
extern char *read_heredoc(const char *delimiter, int expand);

#include <time.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#include "../include/shell.h"
#include "../include/input.h"
#include "../include/alias.h"
#include "../include/rc.h"
#include "../include/plugin.h"
#include "../include/config.h"
#include "../include/security.h"

void signals_init(void);
void jobs_init(void);

void fill_heredocs(CmdList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        Pipeline *p = list->nodes[i].pipeline;
        if (!p) continue;
        for (int j = 0; j < p->ncommands; j++) {
            Command *cmd = &p->commands[j];
            if (cmd->heredoc_delim && !cmd->heredoc_content) {
                cmd->heredoc_content = read_heredoc(
                    cmd->heredoc_delim,
                    cmd->heredoc_expand);
            }
        }
    }
}

int main(void) {
    signals_init();
    jobs_init();

    const char *home = getenv("HOME");

    char history_path[512];
    if (home)
        snprintf(history_path, sizeof(history_path), "%s/.mysh_history", home);
    else
        snprintf(history_path, sizeof(history_path), ".mysh_history");
    history_init(history_path);

    char mysh_dir[512];
    if (home)
        snprintf(mysh_dir, sizeof(mysh_dir), "%s/.mysh", home);
    else
        snprintf(mysh_dir, sizeof(mysh_dir), ".mysh");
    mkdir(mysh_dir, 0755);

    alias_init();

    char config_path[512];
    if (home)
        snprintf(config_path, sizeof(config_path), "%s/.mysh/config", home);
    else
        snprintf(config_path, sizeof(config_path), ".mysh/config");
    config_load(config_path);

    char plugin_dir[512];
    if (home)
        snprintf(plugin_dir, sizeof(plugin_dir), "%s/.mysh/plugins", home);
    else
        snprintf(plugin_dir, sizeof(plugin_dir), ".mysh/plugins");
    plugins_init(plugin_dir);

    char rc_path[512];
    if (home)
        snprintf(rc_path, sizeof(rc_path), "%s/.myshrc", home);
    else
        snprintf(rc_path, sizeof(rc_path), ".myshrc");
    rc_load(rc_path);

    /* Take shell process group ownership */
    pid_t shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    /* ------------------------------------------------------------------ */
    /*  REPL                                                                */
    /* ------------------------------------------------------------------ */
    while (1) {
        /* --- Build prompt --- */
        char prompt[512];
        char cwd[256];
        home = getenv("HOME");  /* refresh each iteration */

        if (getcwd(cwd, sizeof(cwd))) {
            char display[256];
            if (home && strcmp(cwd, home) == 0) {
                strncpy(display, "~", sizeof(display));
            } else {
                char *last = strrchr(cwd, '/');
                if (last && *(last + 1))
                    strncpy(display, last + 1, sizeof(display));
                else
                    strncpy(display, cwd, sizeof(display));
            }

            char time_part[32] = "";
            char user_part[64] = "";

            if (g_config.prompt_show_time) {
                time_t t = time(NULL);
                struct tm *tm = localtime(&t);
                char timebuf[8];
                strftime(timebuf, sizeof(timebuf), "%H:%M", tm);
                snprintf(time_part, sizeof(time_part),
                         "\033[0;37m%s\033[0m ", timebuf);
            }
            if (g_config.prompt_show_user) {
                const char *user = getenv("USER");
                if (user)
                    snprintf(user_part, sizeof(user_part),
                             "\033[1;36m%s\033[0m ", user);
            }

            char *left = hook_prompt_left();
            if (left) {
                snprintf(prompt, sizeof(prompt),
                    "\033[1;32m➜\033[0m  "
                    "%s%s"
                    "\033[1;34m%s\033[0m "
                    "%s\033[0m> ",
                    time_part, user_part, display, left);
                free(left);
            } else {
                snprintf(prompt, sizeof(prompt),
                    "\033[1;32m➜\033[0m  "
                    "%s%s"
                    "\033[1;34m%s\033[0m> ",
                    time_part, user_part, display);
            }
        } else {
            snprintf(prompt, sizeof(prompt), "➜ mysh> ");
        }

        /* --- Read input --- */
        char *input = read_line(prompt);
        if (!input) { printf("\n"); break; }

        input[strcspn(input, "\n")] = '\0';
        if (strlen(input) == 0) { free(input); continue; }

        /* --- Lex --- */
        int ntokens;
        Token *tokens = lex(input, &ntokens);
        if (!tokens) {
            fprintf(stderr, "Error during lexical analysis\n");
            free(input);
            continue;
        }

        int is_assignment = 0;

        /* ---- 1. arr[N]=value — raw input, before any expansion ---- */
        {
            const char *ri = input;
            while (*ri && isspace((unsigned char)*ri)) ri++;
            const char *name_start = ri;
            while (*ri && (isalnum((unsigned char)*ri) || *ri == '_')) ri++;
            if (*ri == '[' && ri > name_start) {
                const char *bracket_open = ri++;          /* skip '[' */
                const char *idx_start    = ri;
                while (*ri && isdigit((unsigned char)*ri)) ri++;
                const char *idx_end = ri;
                if (*ri == ']' && idx_end > idx_start) {
                    ri++;                                  /* skip ']' */
                    if (*ri == '=') {
                        ri++;                             /* skip '=' */
                        char aname[64]   = {0};
                        char idx_buf[16] = {0};
                        strncpy(aname,   name_start,   bracket_open - name_start);
                        strncpy(idx_buf, idx_start,    idx_end - idx_start);
                        arr_set(aname, atoi(idx_buf), ri);
                        is_assignment = 1;
                    }
                }
            }
        }

        /* ---- 2. arr=(a b c) — raw input ---- */
        if (!is_assignment) {
            const char *ri = input;
            while (*ri && isspace((unsigned char)*ri)) ri++;
            const char *name_start = ri;
            while (*ri && (isalnum((unsigned char)*ri) || *ri == '_')) ri++;
            if (*ri == '=' && *(ri + 1) == '(' && ri > name_start) {
                char aname[64] = {0};
                strncpy(aname, name_start, ri - name_start);

                const char *open  = ri + 2;               /* after =( */
                const char *close = strrchr(open, ')');
                if (close) {
                    char content[MAX_INPUT] = {0};
                    strncpy(content, open, close - open);

                    char *elems[256];
                    int   ecount = 0;
                    char *tok2   = strtok(content, " \t\n");
                    while (tok2 && ecount < 256) {
                        elems[ecount++] = tok2;
                        tok2 = strtok(NULL, " \t\n");
                    }
                    arr_set_from_list(aname, elems, ecount);
                    is_assignment = 1;
                }
            }
        }

        /* ---- 3. scalar KEY=VALUE — token-based ---- */
        if (!is_assignment &&
            ntokens == 2 &&
            tokens[0].type == TOK_WORD && tokens[0].value) {
            char *eq = strchr(tokens[0].value, '=');
            /* must have a name before '=' and must NOT be arr[N]= form */
            if (eq && eq != tokens[0].value &&
                strchr(tokens[0].value, '[') == NULL) {
                *eq = '\0';
                local_var_set(tokens[0].value, eq + 1);
                *eq = '=';
                is_assignment = 1;
            }
        }

        if (is_assignment) {
            tokens_free(tokens, ntokens);
            free(input);
            continue;
        }

        /* ---- Expansion pipeline (non-assignments only) ---- */
        tokens = brace_expand_tokens(tokens, &ntokens);
        if (!tokens) { free(input); continue; }
        tokens = glob_expand_tokens(tokens, &ntokens, last_exit_status);
        if (!tokens) { free(input); continue; }
        tokens = word_split_tokens(tokens, ntokens, &ntokens);
        if (!tokens) { free(input); continue; }

        /* ---- Alias expansion + re-lex ---- */
        if (ntokens > 0 &&
            tokens[0].type == TOK_WORD && tokens[0].value) {
            char *expanded = alias_expand(tokens[0].value);
            if (expanded) {
                char combined[4096];
                snprintf(combined, sizeof(combined), "%s", expanded);
                for (int i = 1; i < ntokens - 1; i++) {
                    if (tokens[i].value) {
                        strncat(combined, " ",
                                sizeof(combined) - strlen(combined) - 1);
                        strncat(combined, tokens[i].value,
                                sizeof(combined) - strlen(combined) - 1);
                    }
                }
                tokens_free(tokens, ntokens);
                tokens = lex(combined, &ntokens);
                if (!tokens) { free(input); continue; }
                tokens = brace_expand_tokens(tokens, &ntokens);
                if (!tokens) { free(input); continue; }
                tokens = glob_expand_tokens(tokens, &ntokens, last_exit_status);
                if (!tokens) { free(input); continue; }
            }
        }

        /* ---- Security check ---- */
        const char *sec_reason = NULL;
        SecurityLevel sec_level = security_check(input, &sec_reason);
        int should_run = 1;

        if (sec_level == SEC_BLOCK) {
            fprintf(stderr, "\033[1;31m⛔ Blocked:\033[0m %s\n", sec_reason);
            should_run = 0;
        } else if (sec_level == SEC_WARN) {
            fprintf(stderr,
                "\033[1;33m⚠  Unsafe Command:\033[0m %s\n", sec_reason);
            fprintf(stderr,
                "\033[1;33m   Do you want to continue? [y/N]\033[0m ");
            fflush(stderr);
            char answer[4] = {0};
            if (fgets(answer, sizeof(answer), stdin))
                should_run = (answer[0] == 'y' || answer[0] == 'Y');
            else
                should_run = 0;
        }

        security_audit(input);

        /* ---- Execute ---- */
        if (should_run) {
            CmdList *list = parse_list(tokens, ntokens);
            if (list) {
                fill_heredocs(list);
                execute_list(list);
                cmdlist_free(list);

                /* restore raw mode after execution */
                struct termios raw_restore;
                tcgetattr(STDIN_FILENO, &raw_restore);
                raw_restore.c_lflag &= ~(ICANON | ECHO | ISIG);
                raw_restore.c_cc[VMIN]  = 1;
                raw_restore.c_cc[VTIME] = 0;
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_restore);
            }
        }

        /* ---- Cleanup ---- */
        tokens_free(tokens, ntokens);
        free(input);
    }

    history_close();
    alias_free();
    plugins_unload();
    return 0;
}