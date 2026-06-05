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
