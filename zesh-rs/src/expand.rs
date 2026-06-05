// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// Rust port of the parameter-expansion subset of src/expand.c's expand_word().
//
// Scope & deferral contract
// -------------------------
// This implements exactly the subset that the C expand_word() handles without
// forking or touching the filesystem: literal text, single/double quote
// stripping, $1..$9, $#, $@, $*, $?, $$, $!, $VAR and the full ${...} family
// (`${v}`, `${#v}`, `${v:-w}` `${v:=w}` `${v:+w}` `${v:?w}`, `${v:off:len}`,
// `${v[i]}` `${v[@]}`, `${v@U|L|Q}`).
//
// Anything the C version implements by forking or doing passwd/tilde lookups —
// command substitution `$(...)`, arithmetic `$((...))`, process substitution
// `<(...)` / `>(...)`, tilde `~`, and ANSI-C `$'...'` quoting — is DEFERRED to
// the C implementation: we return NULL with g_expand_error == 0, which the C
// wrapper detects and uses to fall through to expand_word_c_impl(). Deferral is
// always safe because the C implementation is the reference: handing those
// words back to C reproduces its behaviour exactly.
//
// All buffers are Vec<u8>/Vec; the single value returned to C is allocated on
// the C heap (libc malloc) so the caller can free() it.

use core::ptr;
use libc::{c_char, c_int, c_void};

extern "C" {
    fn var_get(name: *const c_char) -> *const c_char;
    fn arr_get(name: *const c_char, index: c_int) -> *const c_char;
    fn arr_len(name: *const c_char) -> c_int;
    fn local_var_set(name: *const c_char, value: *const c_char);
    fn positional_get(idx: c_int) -> *const c_char;
    fn positional_get_count() -> c_int;
    // The C public symbol (the wrapper) — used for recursive expansion of
    // ${var:-WORD} default words, matching the C implementation exactly.
    fn expand_word(word: *const c_char, last_exit_status: c_int) -> *mut c_char;

    static mut g_expand_error: c_int;
    static mut g_last_bg_pid: libc::pid_t;
}

const EMPTY_CSTR: *const c_char = b"\0".as_ptr() as *const c_char;

// ------------------------------------------------------------------ //
//  small helpers                                                       //
// ------------------------------------------------------------------ //

#[inline]
fn is_var_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

#[inline]
fn peek(b: &[u8], k: usize) -> u8 {
    if k < b.len() {
        b[k]
    } else {
        0
    }
}

fn contains2(b: &[u8], a: u8, c: u8) -> bool {
    b.windows(2).any(|w| w[0] == a && w[1] == c)
}

/// Constructs that the C expand_word handles via fork / passwd lookup and that
/// we hand back to C for exact parity.
fn should_defer(b: &[u8]) -> bool {
    if b.len() >= 2 && b[0] == b'$' && b[1] == b'\'' {
        return true; // ANSI-C $'...'
    }
    if b.contains(&b'~') {
        return true; // tilde expansion
    }
    if contains2(b, b'$', b'(') {
        return true; // $( ... )  and  $(( ... ))
    }
    if contains2(b, b'<', b'(') || contains2(b, b'>', b'(') {
        return true; // process substitution
    }
    false
}

unsafe fn cstr_bytes<'a>(p: *const c_char) -> &'a [u8] {
    if p.is_null() {
        return &[];
    }
    core::slice::from_raw_parts(p as *const u8, libc::strlen(p))
}

unsafe fn push_cstr(out: &mut Vec<u8>, p: *const c_char) {
    if !p.is_null() {
        out.extend_from_slice(cstr_bytes(p));
    }
}

unsafe fn strndup_bytes(b: &[u8]) -> *mut c_char {
    let p = libc::malloc(b.len() + 1) as *mut u8;
    if p.is_null() {
        return ptr::null_mut();
    }
    if !b.is_empty() {
        ptr::copy_nonoverlapping(b.as_ptr(), p, b.len());
    }
    *p.add(b.len()) = 0;
    p as *mut c_char
}

/// Mirror `char buf[cap]={0}; if (0<len<cap) strncpy(buf, src, len);` — produces
/// a NUL-terminated buffer; empty when src is empty or would overflow `cap`.
fn ntbuf(src: &[u8], cap: usize) -> Vec<u8> {
    let mut v = Vec::with_capacity(cap);
    if !src.is_empty() && src.len() < cap {
        v.extend_from_slice(src);
    }
    v.push(0);
    v
}

unsafe fn estr(msg: &str) {
    libc::write(2, msg.as_ptr() as *const c_void, msg.len());
}
unsafe fn estr_bytes(b: &[u8]) {
    libc::write(2, b.as_ptr() as *const c_void, b.len());
}

#[inline]
fn push_num(out: &mut Vec<u8>, n: i64) {
    out.extend_from_slice(n.to_string().as_bytes());
}

// ------------------------------------------------------------------ //
//  exported entry point                                                //
// ------------------------------------------------------------------ //

#[no_mangle]
pub unsafe extern "C" fn expand_word_rs(word: *const c_char, last_exit_status: c_int) -> *mut c_char {
    // SAFETY: `word` is NULL or a valid NUL-terminated C string owned by the
    // caller for the duration of the call. On panic we behave like a deferral
    // (NULL + g_expand_error == 0) so C re-runs the word safely.
    let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        expand_impl(word, last_exit_status)
    }));
    match res {
        Ok(p) => p,
        Err(_) => {
            g_expand_error = 0;
            ptr::null_mut()
        }
    }
}

unsafe fn expand_impl(word: *const c_char, last_exit_status: c_int) -> *mut c_char {
    if word.is_null() {
        g_expand_error = 0;
        return ptr::null_mut(); // defer; C impl returns NULL for NULL too
    }
    let len = libc::strlen(word);
    let bytes = core::slice::from_raw_parts(word as *const u8, len);

    if should_defer(bytes) {
        g_expand_error = 0;
        return ptr::null_mut();
    }

    // single-quoted whole word — no expansion
    if len >= 2 && bytes[0] == b'\'' && bytes[len - 1] == b'\'' {
        return strndup_bytes(&bytes[1..len - 1]);
    }

    let mut in_dq = false;
    let mut start = 0usize;
    let mut end = len;
    if len >= 2 && bytes[0] == b'"' && bytes[len - 1] == b'"' {
        in_dq = true;
        start = 1;
        end = len - 1;
    }
    let _ = in_dq;

    let mut out: Vec<u8> = Vec::with_capacity(256);
    let mut idx = start;

    while idx < end {
        let c = bytes[idx];

        if c == b'$' {
            idx += 1;
            let d = peek(bytes, idx);

            // $1 .. $9
            if (b'1'..=b'9').contains(&d) {
                let pidx = (d - b'1') as c_int;
                idx += 1;
                push_cstr(&mut out, positional_get(pidx));
                continue;
            }
            // $#
            if d == b'#' {
                idx += 1;
                push_num(&mut out, positional_get_count() as i64);
                continue;
            }
            // $@  $*
            if d == b'@' || d == b'*' {
                idx += 1;
                let cnt = positional_get_count();
                for pi in 0..cnt {
                    if pi > 0 {
                        out.push(b' ');
                    }
                    push_cstr(&mut out, positional_get(pi));
                }
                continue;
            }
            // $?
            if d == b'?' {
                push_num(&mut out, last_exit_status as i64);
                idx += 1;
                continue;
            }
            // $$
            if d == b'$' {
                #[cfg(feature = "fuzz")]
                {
                    out.extend_from_slice(b"99999");
                }
                #[cfg(not(feature = "fuzz"))]
                {
                    push_num(&mut out, libc::getpid() as i64);
                }
                idx += 1;
                continue;
            }
            // $!
            if d == b'!' {
                push_num(&mut out, g_last_bg_pid as i64);
                idx += 1;
                continue;
            }

            // ${ ... }
            if d == b'{' {
                idx += 1; // past '{'
                let mut var_start = idx;
                let mut get_length = false;
                if peek(bytes, idx) == b'#' && peek(bytes, idx + 1) != b'}' {
                    get_length = true;
                    idx += 1;
                    var_start = idx;
                }
                // read var name — stop at } [ : @ % #
                while idx < len {
                    let ch = bytes[idx];
                    if ch == b'}' || ch == b'[' || ch == b':' || ch == b'@' || ch == b'%' || ch == b'#'
                    {
                        break;
                    }
                    idx += 1;
                }
                let name = &bytes[var_start..idx];
                let name_c = ntbuf(name, 64);
                let name_p = name_c.as_ptr() as *const c_char;

                // ${var@Q|U|L}
                if peek(bytes, idx) == b'@' {
                    idx += 1;
                    let opc = peek(bytes, idx);
                    if opc != 0 {
                        idx += 1;
                    }
                    if peek(bytes, idx) == b'}' {
                        idx += 1;
                    }
                    let v = var_get(name_p);
                    let vb = cstr_bytes(v);
                    match opc {
                        b'Q' => {
                            out.push(b'\'');
                            for &q in vb {
                                if q == b'\'' {
                                    out.extend_from_slice(b"'\\''");
                                } else {
                                    out.push(q);
                                }
                            }
                            out.push(b'\'');
                        }
                        b'U' => {
                            for &q in vb {
                                out.push(q.to_ascii_uppercase());
                            }
                        }
                        b'L' => {
                            for &q in vb {
                                out.push(q.to_ascii_lowercase());
                            }
                        }
                        _ => out.extend_from_slice(vb),
                    }
                    continue;
                }

                // ${var:-w} ${var:=w} ${var:+w} ${var:?w}  (and colon-less forms)
                let pc = peek(bytes, idx);
                if pc == b':'
                    || (pc != b'}'
                        && pc != b'['
                        && (pc == b'-' || pc == b'=' || pc == b'+' || pc == b'?'))
                {
                    let colon = pc == b':';
                    if colon {
                        idx += 1;
                    }
                    let op2 = peek(bytes, idx);
                    if op2 == b'-' || op2 == b'=' || op2 == b'+' || op2 == b'?' {
                        idx += 1;
                        let word_start = idx;
                        let mut bd = 1;
                        while idx < len && bd > 0 {
                            let ch = bytes[idx];
                            if ch == b'{' {
                                bd += 1;
                            } else if ch == b'}' {
                                bd -= 1;
                                if bd == 0 {
                                    break;
                                }
                            }
                            idx += 1;
                        }
                        let wbytes = &bytes[word_start..idx];
                        let word_val = strndup_bytes(wbytes);
                        if peek(bytes, idx) == b'}' {
                            idx += 1;
                        }

                        let cur = var_get(name_p);
                        let is_set = !cur.is_null();
                        let is_empty = cur.is_null() || *cur == 0;
                        let unset_or_empty = if colon { !is_set || is_empty } else { !is_set };

                        let expanded_word = expand_word(word_val, last_exit_status);
                        libc::free(word_val as *mut c_void);
                        let use_b = cstr_bytes(expanded_word);
                        let use_ptr = if expanded_word.is_null() {
                            EMPTY_CSTR
                        } else {
                            expanded_word as *const c_char
                        };

                        match op2 {
                            b'-' => {
                                if unset_or_empty {
                                    out.extend_from_slice(use_b);
                                } else {
                                    push_cstr(&mut out, cur);
                                }
                            }
                            b'=' => {
                                if unset_or_empty {
                                    local_var_set(name_p, use_ptr);
                                    libc::setenv(name_p, use_ptr, 1);
                                    out.extend_from_slice(use_b);
                                } else {
                                    push_cstr(&mut out, cur);
                                }
                            }
                            b'+' => {
                                if !unset_or_empty {
                                    out.extend_from_slice(use_b);
                                }
                            }
                            b'?' => {
                                if unset_or_empty {
                                    estr("zesh: ");
                                    estr_bytes(name);
                                    estr(": ");
                                    if !use_b.is_empty() {
                                        estr_bytes(use_b);
                                    } else {
                                        estr("parameter null or not set");
                                    }
                                    estr("\n");
                                    if !expanded_word.is_null() {
                                        libc::free(expanded_word as *mut c_void);
                                    }
                                    g_expand_error = 1;
                                    return ptr::null_mut();
                                } else {
                                    push_cstr(&mut out, cur);
                                }
                            }
                            _ => {}
                        }
                        if !expanded_word.is_null() {
                            libc::free(expanded_word as *mut c_void);
                        }
                        continue;
                    }
                    // colon but not a modifier — plain ${var:}
                    if colon && peek(bytes, idx) == b'}' {
                        idx += 1;
                        let v = var_get(name_p);
                        if !v.is_null() {
                            push_cstr(&mut out, v);
                        }
                        continue;
                    }
                    if colon {
                        idx -= 1; // restore consumed ':'
                    }
                }

                // substring ${var:offset:length}
                if peek(bytes, idx) == b':' {
                    let nxt = peek(bytes, idx + 1);
                    if (b'0'..=b'9').contains(&nxt) {
                        idx += 1; // skip ':'
                        let mut offset: i64 = 0;
                        let mut neg_off = false;
                        if peek(bytes, idx) == b'-' {
                            neg_off = true;
                            idx += 1;
                        }
                        while (b'0'..=b'9').contains(&peek(bytes, idx)) {
                            offset = offset * 10 + (peek(bytes, idx) - b'0') as i64;
                            idx += 1;
                        }
                        let mut length: i64 = -1;
                        if peek(bytes, idx) == b':' {
                            idx += 1;
                            length = 0;
                            while (b'0'..=b'9').contains(&peek(bytes, idx)) {
                                length = length * 10 + (peek(bytes, idx) - b'0') as i64;
                                idx += 1;
                            }
                        }
                        if peek(bytes, idx) == b'}' {
                            idx += 1;
                        }
                        let v = var_get(name_p);
                        if !v.is_null() {
                            let vb = cstr_bytes(v);
                            let vlen = vb.len() as i64;
                            if neg_off {
                                offset = vlen - offset;
                            }
                            if offset < 0 {
                                offset = 0;
                            }
                            if offset > vlen {
                                offset = vlen;
                            }
                            let avail = vlen - offset;
                            let mut take = if length < 0 { avail } else { length };
                            if take > avail {
                                take = avail;
                            }
                            if take < 0 {
                                take = 0;
                            }
                            let o = offset as usize;
                            out.extend_from_slice(&vb[o..o + take as usize]);
                        }
                        continue;
                    }
                }

                // array index ${arr[N]} ${arr[@]} ${arr[*]}
                if peek(bytes, idx) == b'[' {
                    idx += 1;
                    let idx_start = idx;
                    while idx < len && bytes[idx] != b']' {
                        idx += 1;
                    }
                    let idx_bytes = &bytes[idx_start..idx];
                    let idx_trunc: &[u8] = if !idx_bytes.is_empty() && idx_bytes.len() < 32 {
                        idx_bytes
                    } else {
                        &[]
                    };
                    if peek(bytes, idx) == b']' {
                        idx += 1;
                    }
                    if peek(bytes, idx) == b'}' {
                        idx += 1;
                    }
                    let is_at_star = idx_trunc == b"@" || idx_trunc == b"*";

                    if get_length {
                        if is_at_star {
                            push_num(&mut out, arr_len(name_p) as i64);
                        } else {
                            let abuf = ntbuf(idx_bytes, 32);
                            let ai = libc::atoi(abuf.as_ptr() as *const c_char);
                            let v = arr_get(name_p, ai);
                            let l = if v.is_null() { 0 } else { libc::strlen(v) };
                            push_num(&mut out, l as i64);
                        }
                    } else if is_at_star {
                        let alen = arr_len(name_p);
                        for ai in 0..alen {
                            let v = arr_get(name_p, ai);
                            if ai > 0 {
                                out.push(b' ');
                            }
                            push_cstr(&mut out, v);
                        }
                    } else {
                        let abuf = ntbuf(idx_bytes, 32);
                        let ai = libc::atoi(abuf.as_ptr() as *const c_char);
                        let v = arr_get(name_p, ai);
                        push_cstr(&mut out, v);
                    }
                    continue;
                }

                // plain ${VAR} or ${#VAR}
                if peek(bytes, idx) == b'}' {
                    idx += 1;
                    if get_length {
                        let v = var_get(name_p);
                        let l = if v.is_null() { 0 } else { libc::strlen(v) };
                        push_num(&mut out, l as i64);
                    } else {
                        let v = var_get(name_p);
                        push_cstr(&mut out, v);
                    }
                    continue;
                }

                // malformed — reprocess from '{' literally (mirror C rewind)
                let back = if get_length { 2 } else { 1 };
                idx = var_start - back;
                continue;
            }

            // $VAR — unbracketed
            if is_var_char(d) {
                let vstart = idx;
                while idx < len && is_var_char(bytes[idx]) {
                    idx += 1;
                }
                let name = &bytes[vstart..idx];
                if !name.is_empty() {
                    let name_c = ntbuf(name, name.len() + 1);
                    let v = var_get(name_c.as_ptr() as *const c_char);
                    push_cstr(&mut out, v);
                    continue;
                }
            }

            // lone '$'
            out.push(b'$');
            continue;
        }

        // regular character
        out.push(c);
        idx += 1;
    }

    strndup_bytes(&out)
}
