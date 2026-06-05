#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include "fuzz_common.h"
#include "../include/shell.h"

extern void tokens_free(Token *toks, int n);
extern Token *glob_expand_tokens(Token *toks, int *ntokens, int last_exit);

extern int g_opt_errexit;
extern int g_opt_xtrace;
extern int g_opt_pipefail;
extern int g_returning;
extern int g_return_value;
extern volatile int g_interrupt_loop;
extern int g_expand_error;
extern int g_is_subshell;
extern int g_current_lineno;
extern LoopControl g_loop_control;
extern volatile sig_atomic_t g_sigint_received;
extern char  g_current_funcname[256];
extern char  g_current_source[4096];
extern pid_t g_last_bg_pid;
extern char *g_trap_actions[TRAP_NSIG];
extern char *g_trap_exit;
extern int   g_declared_var_count;

static void reset_globals(void)
{
    g_opt_errexit     = 0;
    g_opt_xtrace      = 0;
    g_opt_pipefail    = 0;
    g_returning       = 0;
    g_return_value    = 0;
    g_interrupt_loop  = 0;
    g_expand_error    = 0;
    g_is_subshell     = 0;
    g_loop_control    = LOOP_NORMAL;
    g_sigint_received = 0;
    g_current_lineno  = 0;
    g_last_bg_pid     = 0;
    g_declared_var_count = 0;
    g_current_funcname[0] = '\0';
    g_current_source[0]   = '\0';
    for (int i = 0; i < TRAP_NSIG; i++) { free(g_trap_actions[i]); g_trap_actions[i] = NULL; }
    free(g_trap_exit); g_trap_exit = NULL;
    func_free_all();
    memset(g_declared_vars, 0, sizeof(g_declared_vars));
    g_shell_start_time = 1; /* FIX: fixed time prevents $SECONDS non-determinism */
}

static void on_alarm(int sig) { (void)sig; _exit(1); }

static void fuzz_setup(void)
{
    setenv("PATH", "", 1);
    srand(0); /* FIX: deterministic rand() so $RANDOM expansion is reproducible */
    mkdir("/tmp/zesh_fuzz_script_empty", 0700);
    if (chdir("/tmp/zesh_fuzz_script_empty") != 0) chdir("/tmp");
}

int main(int argc, char *argv[])
{
    fuzz_setup();
    FUZZ_INIT();

    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    char buf[FUZZ_BUF_SIZE];

    while (FUZZ_LOOP_FORK()) {
        size_t n = fuzz_read_one(buf, argc, argv);
        if (n == 0) continue;

        reset_globals();

        alarm(3);

        int ntokens = 0;
        Token *toks = lex(buf, &ntokens);
        if (!toks) { alarm(0); continue; }

        toks = glob_expand_tokens(toks, &ntokens, 0);
        if (!toks) { alarm(0); continue; }

        CmdList *list = parse_list(toks, ntokens);
        tokens_free(toks, ntokens);

        if (!list) { alarm(0); continue; }

        /* FIX: unique workdir per iteration via mkdtemp — inner child's files don't affect others */
        char wdir[] = "/tmp/zs_XXXXXX";
        mkdtemp(wdir); /* ignore failure — falls back to cwd */

        pid_t pid = fork();
        if (pid == 0) {
            if (wdir[0]) chdir(wdir); /* inner child works in fresh dir */
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) {
                dup2(dn, STDIN_FILENO);
                dup2(dn, STDOUT_FILENO);
                dup2(dn, STDERR_FILENO);
                close(dn);
            }
            alarm(1);
            execute_list(list);
            cmdlist_free(list);
            ps_fds_close();
            ps_pids_wait();
            func_free_all();
            for (int si = 0; si < TRAP_NSIG; si++) { free(g_trap_actions[si]); g_trap_actions[si] = NULL; }
            free(g_trap_exit); g_trap_exit = NULL;
            _exit(0);

        } else if (pid > 0) {

            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
            cmdlist_free(list);

        } else {
            cmdlist_free(list);
        }

        alarm(0);
    }
    return 0;
}

