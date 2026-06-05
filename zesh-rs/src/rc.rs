// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// rc.rs — Rust reimplementation of src/rc.c (rc_load).
//
// Loads ~/.zeshrc line by line: skips blanks/comments, handles `alias name=val`
// definitions directly (so they aren't executed), and runs every other line
// through the existing pipeline (lex -> glob_expand_tokens -> parse_list ->
// execute_list). Those pipeline functions are the bare C symbols (which already
// dispatch to Rust when their own USE_RUST_* guard is on), so this module is
// pure orchestration and stays correct regardless of which other modules are
// in Rust.

use libc::{c_char, c_int, c_void};

use crate::ffi::{last_exit, CmdList, Token};

extern "C" {
    fn lex(input: *const c_char, ntokens: *mut c_int) -> *mut Token;
    fn tokens_free(toks: *mut Token, n: c_int);
    fn parse_list(toks: *mut Token, ntokens: c_int) -> *mut CmdList;
    fn cmdlist_free(list: *mut CmdList);
    fn glob_expand_tokens(toks: *mut Token, ntokens: *mut c_int, last: c_int) -> *mut Token;
    fn execute_list(list: *mut CmdList) -> c_int;
    fn alias_add(name: *const c_char, value: *const c_char);
}

#[inline]
fn is_ws(c: u8) -> bool {
    c == b' ' || c == b'\t'
}

/// NUL-terminate a byte slice into a fresh Vec for passing to C.
fn cstr(bytes: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(bytes.len() + 1);
    v.extend_from_slice(bytes);
    v.push(0);
    v
}

/// Handle one already-trimmed `alias name=value` line (leading-ws-stripped `p`).
/// Mirrors rc.c's name/value extraction and quote stripping.
unsafe fn handle_alias(p: &[u8]) {
    // p starts with "alias "
    let alias_part = &p[6..];
    let eq = match alias_part.iter().position(|&c| c == b'=') {
        Some(i) => i,
        None => return,
    };
    // NAME: between "alias " and '=', trimmed of surrounding whitespace.
    let mut name = &alias_part[..eq];
    while !name.is_empty() && is_ws(name[0]) {
        name = &name[1..];
    }
    while !name.is_empty() && is_ws(name[name.len() - 1]) {
        name = &name[..name.len() - 1];
    }
    // VALUE: everything after '=', leading whitespace stripped.
    let mut value = &alias_part[eq + 1..];
    while !value.is_empty() && is_ws(value[0]) {
        value = &value[1..];
    }
    // Strip surrounding matching quotes, repeatedly (matches rc.c's while loop).
    while !value.is_empty() && (value[0] == b'\'' || value[0] == b'"') {
        let q = value[0];
        if value.len() >= 2 && value[value.len() - 1] == q {
            value = &value[1..value.len() - 1];
        } else {
            break;
        }
    }
    let name_z = cstr(name);
    let value_z = cstr(value);
    alias_add(name_z.as_ptr() as *const c_char, value_z.as_ptr() as *const c_char);
}

/// Execute one line through the standard pipeline (lex/glob/parse/execute).
unsafe fn execute_line(p: &[u8]) {
    let pz = cstr(p);
    let mut ntokens: c_int = 0;
    let toks = lex(pz.as_ptr() as *const c_char, &mut ntokens);
    if toks.is_null() {
        return;
    }
    let toks = glob_expand_tokens(toks, &mut ntokens, last_exit());
    if toks.is_null() {
        return;
    }
    let list = parse_list(toks, ntokens);
    if !list.is_null() {
        execute_list(list);
        cmdlist_free(list);
    }
    tokens_free(toks, ntokens);
}

#[no_mangle]
pub unsafe extern "C" fn rc_load_rs(path: *const c_char) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if path.is_null() {
            return;
        }
        let f = libc::fopen(path, b"r\0".as_ptr() as *const c_char);
        if f.is_null() {
            return; // file may not exist
        }
        let mut data: Vec<u8> = Vec::new();
        let mut buf = [0u8; 4096];
        loop {
            let n = libc::fread(buf.as_mut_ptr() as *mut c_void, 1, buf.len(), f);
            if n == 0 {
                break;
            }
            data.extend_from_slice(&buf[..n]);
        }
        libc::fclose(f);

        for raw in data.split(|&c| c == b'\n') {
            // Strip trailing newline/space/tab (newline already removed by split).
            let mut line = raw;
            while !line.is_empty()
                && (line[line.len() - 1] == b'\n' || is_ws(line[line.len() - 1]))
            {
                line = &line[..line.len() - 1];
            }
            if line.is_empty() {
                continue;
            }
            // Leading whitespace strip for the comment/alias checks.
            let mut p = line;
            while !p.is_empty() && is_ws(p[0]) {
                p = &p[1..];
            }
            if p.is_empty() || p[0] == b'#' {
                continue;
            }
            if p.starts_with(b"alias ") {
                handle_alias(p);
                continue;
            }
            execute_line(p);
        }
    }));
}
