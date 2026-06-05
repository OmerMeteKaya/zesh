/* AUTO-GENERATED equivalent — hand-maintained C declarations for the zesh-rs
 * static library (the zesh-rs crate). Kept in sync with the #[no_mangle]
 * extern "C" entry points; the Rust side mirrors include/shell.h's structs
 * with #[repr(C)] and compile-time size assertions. */
#ifndef ZESH_RS_H
#define ZESH_RS_H

#include "shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/* lexer.rs — tokenizer. Returns a C-heap Token array (each `value` malloc'd);
 * free with tokens_free_rs (or the C tokens_free, which is layout-compatible). */
Token *lex_rs(const char *input, int *ntokens);
void   tokens_free_rs(Token *toks, int n);

/* alias.rs — alias table (Mutex-guarded). Same symbols as include/alias.h,
 * routed by the C #ifdef USE_RUST_ALIAS wrappers in src/alias.c. */
void  alias_init_rs(void);
void  alias_add_rs(const char *name, const char *value);
void  alias_remove_rs(const char *name);
char *alias_expand_rs(const char *name);
void  alias_list_rs(void);
void  alias_free_rs(void);
void  alias_each_rs(void (*cb)(const char *name, const char *value, void *ud), void *ud);

/* rc.rs — ~/.zeshrc loader (orchestrates lex/parse/execute + alias defs). */
void rc_load_rs(const char *path);

/* config.rs — ~/.zesh/config parser/serializer with corrupt-proof schema
 * validation. Reads/writes the C-owned g_config. */
void config_load_rs(const char *path);
void config_save_rs(const char *path);

/* security.rs — dangerous-command detection + audit log. Reads g_config.
 * security_check_rs returns SecurityLevel-as-int (0=OK,1=WARN,2=BLOCK). */
int  security_check_rs(const char *cmdline, const char **reason);
void security_audit_rs(const char *cmdline);
void security_init_rs(void);

/* parser.rs — list parser (delegates each pipeline leaf to the C parse()). */
CmdList *parse_list_rs(Token *toks, int ntokens);
void     cmdlist_free_rs(CmdList *list);

/* expand.rs — parameter expansion. Returns a malloc'd string the caller must
 * free(). Returns NULL with g_expand_error == 0 to signal "not handled by Rust;
 * fall through to the C implementation" (command/process/arith substitution,
 * tilde, ANSI-C quoting). */
char *expand_word_rs(const char *word, int last_exit_status);

#ifdef __cplusplus
}
#endif

#endif /* ZESH_RS_H */
