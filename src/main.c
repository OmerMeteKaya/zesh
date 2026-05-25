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
int run_script_line(const char *input) {
    if (!input || strlen(input) == 0) return 0;

    /* scalar assignment */
    {
        const char *ri = input;
        while (*ri && isspace((unsigned char)*ri)) ri++;
        const char *name_start = ri;
        while (*ri && (isalnum((unsigned char)*ri) || *ri == '_')) ri++;
        if (*ri == '=' && *(ri+1) != '(' && ri > name_start &&
    strchr(ri, '[') == NULL &&
    strchr(input, ';') == NULL) {
            /* name=value */
            char name[64] = {0};
            strncpy(name, name_start, ri - name_start);
            ri++; /* skip = */
            int ntmp;
            Token *ttmp = lex(ri, &ntmp);
            char *expanded = ttmp ? expand_word(ri, last_exit_status) : strdup(ri);
            if (ttmp) tokens_free(ttmp, ntmp);
            local_var_set(name, expanded ? expanded : ri);
            free(expanded);
            return 0;
        }
    }

    /* arr=(a b c) */
    {
        const char *ri = input;
        while (*ri && isspace((unsigned char)*ri)) ri++;
        const char *name_start = ri;
        while (*ri && (isalnum((unsigned char)*ri) || *ri == '_')) ri++;
        if (*ri == '=' && *(ri+1) == '(' && ri > name_start) {
            char aname[64] = {0};
            strncpy(aname, name_start, ri - name_start);
            const char *open  = ri + 2;
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
            }
            return 0;
        }
    }

    int ntokens;
    Token *tokens = lex(input, &ntokens);
    if (!tokens) return 1;

    int has_compound = 0;
    for (int ci = 0; ci < ntokens; ci++) {
        if (tokens[ci].type == TOK_WORD && tokens[ci].value) {
            const char *v = tokens[ci].value;
            if (strcmp(v, "if")    == 0 || strcmp(v, "while") == 0 ||
                strcmp(v, "until") == 0 || strcmp(v, "for")   == 0 || strcmp(v, "case") == 0) {
                has_compound = 1;
                break;
                }
            size_t vlen = strlen(v);
            if (vlen >= 3 &&
                v[vlen-2] == '(' && v[vlen-1] == ')') {
                has_compound = 1;
                break;
                }
            if (ci + 2 < ntokens &&
                tokens[ci+1].value && strcmp(tokens[ci+1].value, "(") == 0 &&
                tokens[ci+2].value && strcmp(tokens[ci+2].value, ")") == 0) {
                has_compound = 1;
                break;
                }
        }
    }

    if (!has_compound) {
        tokens = brace_expand_tokens(tokens, &ntokens);
        if (!tokens) return 1;
        tokens = glob_expand_tokens(tokens, &ntokens, last_exit_status);
        if (!tokens) return 1;
        tokens = word_split_tokens(tokens, ntokens, &ntokens);
        if (!tokens) return 1;
    } else {
        tokens = brace_expand_tokens(tokens, &ntokens);
        if (!tokens) return 1;
    }

    /* alias expansion */
    if (!has_compound &&
        ntokens > 0 &&
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
            if (!tokens) return 1;
            tokens = brace_expand_tokens(tokens, &ntokens);
            if (!tokens) return 1;
            tokens = glob_expand_tokens(tokens, &ntokens, last_exit_status);
            if (!tokens) return 1;
        }
    }

    CmdList *list = parse_list(tokens, ntokens);
    int status = 0;
    if (list) {
        fill_heredocs(list);
        status = execute_list(list);
        cmdlist_free(list);
    }

    tokens_free(tokens, ntokens);
    return status;
}
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

int main(int argc, char *argv[]) {
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
    if (argc >= 2) {
        FILE *f = fopen(argv[1], "r");
        if (!f) {
            fprintf(stderr, "mysh: %s: cannot open file\n", argv[1]);
            return 1;
        }

        char line[MAX_INPUT];
        char collected[65536] = {0};
        int  collecting = 0;
        int  depth      = 0;
        int  exit_status = 0;

        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == '\0') continue;

            line[strcspn(line, "\n")] = '\0';

            {
                const char *tr = line;
                while (*tr == ' ' || *tr == '\t') tr++;
                size_t tlen;

                tlen = strlen("if");
                if (strncmp(tr, "if", tlen) == 0 &&
                    (tr[tlen]==' '||tr[tlen]=='\t'||
                     tr[tlen]=='\0'||tr[tlen]=='\n')) depth++;

                tlen = strlen("while");
                if (strncmp(tr, "while", tlen) == 0 &&
                    (tr[tlen]==' '||tr[tlen]=='\t'||
                     tr[tlen]=='\0'||tr[tlen]=='\n')) depth++;

                tlen = strlen("until");
                if (strncmp(tr, "until", tlen) == 0 &&
                    (tr[tlen]==' '||tr[tlen]=='\t'||
                     tr[tlen]=='\0'||tr[tlen]=='\n')) depth++;

                tlen = strlen("for");
                if (strncmp(tr, "for", tlen) == 0 &&
                    (tr[tlen]==' '||tr[tlen]=='\t'||
                     tr[tlen]=='\0'||tr[tlen]=='\n')) depth++;

                tlen = strlen("fi");
                if (strncmp(tr, "fi", tlen) == 0 &&
                    (tr[tlen]==' '||tr[tlen]=='\t'||
                     tr[tlen]=='\0'||tr[tlen]=='\n')) depth--;

                tlen = strlen("done");
                if (strncmp(tr, "done", tlen) == 0 &&
                    (tr[tlen]==' '||tr[tlen]=='\t'||
                     tr[tlen]=='\0'||tr[tlen]=='\n')) depth--;

                tlen = strlen("case");
                if (strncmp(tr, "case", tlen) == 0 &&
                    (tr[tlen]==' '||tr[tlen]=='\t'||
                     tr[tlen]=='\0'||tr[tlen]=='\n')) depth++;

                tlen = strlen("esac");
                if (strncmp(tr, "esac", tlen) == 0 &&
                    (tr[tlen]==' '||tr[tlen]=='\t'||
                     tr[tlen]=='\0'||tr[tlen]=='\n')) depth--;
                
                {
                    const char *tr2 = tr;
                    while (*tr2 && (isalnum((unsigned char)*tr2) || *tr2 == '_')) tr2++;
                    if (*tr2 == '(' && *(tr2+1) == ')') depth++;
                }
            }

            if (!collecting && depth > 0) {
                collecting = 1;
                strncpy(collected, line, sizeof(collected) - 1);
                strncat(collected, " ; ",
                        sizeof(collected) - strlen(collected) - 1);
                continue;
            }

            if (collecting) {
                strncat(collected, line,
                        sizeof(collected) - strlen(collected) - 1);
                strncat(collected, " ; ",
                        sizeof(collected) - strlen(collected) - 1);

                if (depth <= 0) {
                    collecting = 0;
                    depth      = 0;
                    exit_status = run_script_line(collected);
                    memset(collected, 0, sizeof(collected));
                }
                continue;
            }

            exit_status = run_script_line(line);
        }

        fclose(f);
        return exit_status;
    }
/* ---------------------------------------------------------------- */
    /*  REPL                                                             */
    /* ---------------------------------------------------------------- */
    while (1) {
       int from_block = 0;
        /* --- Build prompt --- */
        char prompt[512];
        char cwd[256];
        home = getenv("HOME");
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

        /* --- Read input (multiline collector) --- */
        char *input = read_line(prompt);
        if (!input) { printf("\n"); break; }

        /* --- Lex --- */
        int ntokens;
        Token *tokens = lex(input, &ntokens);
        if (!tokens) {
            fprintf(stderr, "Error during lexical analysis\n");
            free(input);
            continue;
        }

        int is_assignment = 0;

        /* ---- 1. arr[N]=value ---- */
        if (!from_block) {
            {
                const char *ri = input;
                while (*ri && isspace((unsigned char)*ri)) ri++;
                const char *name_start = ri;
                while (*ri && (isalnum((unsigned char)*ri) || *ri == '_')) ri++;
                if (*ri == '[' && ri > name_start) {
                    const char *bracket_open = ri++;
                    const char *idx_start    = ri;
                    while (*ri && isdigit((unsigned char)*ri)) ri++;
                    const char *idx_end = ri;
                    if (*ri == ']' && idx_end > idx_start) {
                        ri++;
                        if (*ri == '=') {
                            ri++;
                            char aname[64]   = {0};
                            char idx_buf[16] = {0};
                            strncpy(aname,   name_start,
                                    bracket_open - name_start);
                            strncpy(idx_buf, idx_start,
                                    idx_end - idx_start);
                            arr_set(aname, atoi(idx_buf), ri);
                            is_assignment = 1;
                        }
                    }
                }
            }

            /* ---- 2. arr=(a b c) ---- */
            if (!is_assignment) {
                const char *ri = input;
                while (*ri && isspace((unsigned char)*ri)) ri++;
                const char *name_start = ri;
                while (*ri && (isalnum((unsigned char)*ri) || *ri == '_')) ri++;
                if (*ri == '=' && *(ri + 1) == '(' && ri > name_start) {
                    char aname[64] = {0};
                    strncpy(aname, name_start, ri - name_start);
                    const char *open  = ri + 2;
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

            /* ---- 3. scalar KEY=VALUE ---- */
            if (!is_assignment &&
        tokens[0].type == TOK_WORD && tokens[0].value) {
                char *eq = strchr(tokens[0].value, '=');
                if (eq && eq != tokens[0].value &&
                    strchr(tokens[0].value, '[') == NULL &&
                    ntokens <= 2) {
                    *eq = '\0';
                    char *raw_val = eq + 1;
                    /* arithmetic expand et */
                    char *expanded_val = expand_word(raw_val, last_exit_status);
                    local_var_set(tokens[0].value, expanded_val ? expanded_val : raw_val);
                    if (expanded_val) free(expanded_val);
                    *eq = '=';
                    is_assignment = 1;
                    }
        }
        }

        if (is_assignment) {
            tokens_free(tokens, ntokens);
            free(input);
            continue;
        }

        /* ---- Expansion pipeline ---- */
        int has_compound = 0;
        for (int ci = 0; ci < ntokens; ci++) {
            if (tokens[ci].type == TOK_WORD && tokens[ci].value) {
                const char *v = tokens[ci].value;
                if (strcmp(v, "if")    == 0 || strcmp(v, "while") == 0 ||
                    strcmp(v, "until") == 0 || strcmp(v, "for")   == 0 || strcmp(v, "case") == 0) {
                    has_compound = 1;
                    break;
                    }
                size_t vlen = strlen(v);
                if (vlen >= 3 &&
                    v[vlen-2] == '(' && v[vlen-1] == ')') {
                    has_compound = 1;
                    break;
                    }
                if (ci + 2 < ntokens &&
                    tokens[ci+1].value && strcmp(tokens[ci+1].value, "(") == 0 &&
                    tokens[ci+2].value && strcmp(tokens[ci+2].value, ")") == 0) {
                    has_compound = 1;
                    break;
                    }
            }
        }

        if (!has_compound) {
            tokens = brace_expand_tokens(tokens, &ntokens);
            if (!tokens) { free(input); continue; }
            tokens = glob_expand_tokens(tokens, &ntokens, last_exit_status);
            if (!tokens) { free(input); continue; }
            tokens = word_split_tokens(tokens, ntokens, &ntokens);
            if (!tokens) { free(input); continue; }
        } else {
            tokens = brace_expand_tokens(tokens, &ntokens);
            if (!tokens) { free(input); continue; }
        }

        /* ---- Alias expansion ---- */
        if (!has_compound &&
    ntokens > 0 &&
    tokens[0].type == TOK_WORD && tokens[0].value) {
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
                    tokens = glob_expand_tokens(tokens, &ntokens,
                                                last_exit_status);
                    if (!tokens) { free(input); continue; }
                }
                }
        }

        /* ---- Security check ---- */
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

                struct termios exec_term;
                tcgetattr(STDIN_FILENO, &exec_term);
                exec_term.c_lflag |= ISIG;   /* Ctrl+C → SIGINT */
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &exec_term);

                g_interrupt_loop = 0;
                int exec_status = execute_list(list);
                if (exec_status == 130) {
                    write(STDOUT_FILENO, "\r\n", 2);
                }
                if (g_opt_errexit && exec_status != 0 &&
                    exec_status != 130) {
                    /* restore terminal then run EXIT trap and quit */
                    struct termios t;
                    if (tcgetattr(STDIN_FILENO, &t) == 0) {
                        t.c_lflag |= (ICANON | ECHO | ISIG);
                        t.c_iflag |= ICRNL;
                        tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
                    }
                    history_close();
                    alias_free();
                    plugins_unload();
                    trap_run_exit(exec_status);
                    }
                g_sigint_received = 0;
                g_interrupt_loop  = 0;

                struct termios raw_restore;
                tcgetattr(STDIN_FILENO, &raw_restore);
                raw_restore.c_lflag &= ~(ICANON | ECHO | ISIG);
                raw_restore.c_cc[VMIN]  = 1;
                raw_restore.c_cc[VTIME] = 0;
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_restore);

                cmdlist_free(list);
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