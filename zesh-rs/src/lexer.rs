// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// lexer.rs — a faithful Rust reimplementation of src/lexer.c's `lex()`.
//
// Behavioural contract: identical tokenization to the C lexer for every input
// that test_parite.sh and the fuzz corpus exercise. The one deliberate
// difference is that token-value accumulation uses growable `Vec<u8>` buffers
// instead of the C side's fixed-size stack arrays (hsbuf[MAX_INPUT],
// combined[MAX_INPUT*2]); this means the Rust lexer does NOT silently truncate
// oversized here-strings / quoted suffixes the way C does. For all inputs below
// those limits (everything in the test/fuzz corpora) the output is identical.
//
// Memory ownership: the returned Token array and every `value` string are
// allocated on the C heap (libc::malloc) so the existing C `tokens_free()`
// frees them correctly. Both `lex_rs` and `tokens_free_rs` are exported; the C
// `#ifdef USE_RUST_LEXER` wrappers route `lex`/`tokens_free` here.

use libc::{c_char, c_int, c_void};
use core::ptr;

use crate::ffi::{tok, Token};

// ---- byte-class helpers matching C <ctype.h> in the "C" locale ----

#[inline]
fn at(b: &[u8], i: usize) -> u8 {
    // Past the logical end of the C string reads as NUL, mirroring `*p` after
    // the terminating '\0' (the slice excludes the NUL itself).
    if i < b.len() { b[i] } else { 0 }
}

#[inline]
fn is_space(c: u8) -> bool {
    // C isspace() in the "C" locale: space, \t, \n, \v, \f, \r.
    matches!(c, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

#[inline]
fn is_digit(c: u8) -> bool {
    c.is_ascii_digit()
}

#[inline]
fn hex_val(c: u8) -> u32 {
    if c.is_ascii_digit() {
        (c - b'0') as u32
    } else {
        (c.to_ascii_lowercase() - b'a' + 10) as u32
    }
}

// ---- accumulated token (materialized to the C heap at the end) ----

struct RawTok {
    type_: c_int,
    value: Option<Vec<u8>>, // None => NULL value (operators)
    quoted: c_int,
}

impl RawTok {
    fn op(type_: c_int) -> RawTok {
        RawTok { type_, value: None, quoted: 0 }
    }
    fn word(type_: c_int, value: Vec<u8>) -> RawTok {
        RawTok { type_, value: Some(value), quoted: 0 }
    }
}

/// Allocate a NUL-terminated C string on the C heap from raw bytes.
/// SAFETY: caller owns the returned pointer (freed by C `free`).
unsafe fn c_strdup_bytes(bytes: &[u8]) -> *mut c_char {
    let p = libc::malloc(bytes.len() + 1) as *mut u8;
    if p.is_null() {
        return ptr::null_mut();
    }
    ptr::copy_nonoverlapping(bytes.as_ptr(), p, bytes.len());
    *p.add(bytes.len()) = 0;
    p as *mut c_char
}

/// Materialize the accumulated tokens into a C-heap Token array.
unsafe fn materialize(toks: Vec<RawTok>) -> *mut Token {
    let n = toks.len();
    let arr = libc::malloc(n * core::mem::size_of::<Token>()) as *mut Token;
    if arr.is_null() {
        return ptr::null_mut();
    }
    for (i, t) in toks.into_iter().enumerate() {
        let value = match t.value {
            Some(v) => c_strdup_bytes(&v),
            None => ptr::null_mut(),
        };
        ptr::write(
            arr.add(i),
            Token { type_: t.type_, value, quoted: t.quoted },
        );
    }
    arr
}

// ---- core lexer ----

fn lex_internal(b: &[u8]) -> Vec<RawTok> {
    let mut toks: Vec<RawTok> = Vec::with_capacity(16);
    let mut i: usize = 0;

    'main: while at(b, i) != 0 {
        let c = at(b, i);

        if c == b'\n' {
            toks.push(RawTok::op(tok::SEMI));
            i += 1;
            continue;
        }
        if is_space(c) {
            i += 1;
            continue;
        }

        // ---- single/double-character operators ----
        match c {
            b'&' => {
                if at(b, i + 1) == b'&' {
                    toks.push(RawTok::op(tok::AND));
                    i += 2;
                } else {
                    toks.push(RawTok::op(tok::BG));
                    i += 1;
                }
                continue;
            }
            b'|' => {
                if at(b, i + 1) == b'|' {
                    toks.push(RawTok::op(tok::OR));
                    i += 2;
                } else {
                    toks.push(RawTok::op(tok::PIPE));
                    i += 1;
                }
                continue;
            }
            b';' => {
                toks.push(RawTok::op(tok::SEMI));
                i += 1;
                continue;
            }
            b'[' => {
                if at(b, i + 1) == b'[' {
                    toks.push(RawTok::op(tok::DOUBLE_LBRACKET));
                    i += 2;
                } else {
                    toks.push(RawTok::word(tok::WORD, b"[".to_vec()));
                    i += 1;
                }
                continue;
            }
            b']' => {
                if at(b, i + 1) == b']' {
                    toks.push(RawTok::op(tok::DOUBLE_RBRACKET));
                    i += 2;
                } else {
                    toks.push(RawTok::word(tok::WORD, b"]".to_vec()));
                    i += 1;
                }
                continue;
            }
            b'{' => {
                toks.push(RawTok::word(tok::WORD, b"{".to_vec()));
                i += 1;
                continue;
            }
            b'}' => {
                toks.push(RawTok::word(tok::WORD, b"}".to_vec()));
                i += 1;
                continue;
            }
            b'(' => {
                if at(b, i + 1) == b'(' {
                    toks.push(RawTok::op(tok::DOUBLE_LPAREN));
                    i += 2;
                } else {
                    toks.push(RawTok::word(tok::WORD, b"(".to_vec()));
                    i += 1;
                }
                continue;
            }
            b')' => {
                if at(b, i + 1) == b')' {
                    toks.push(RawTok::op(tok::DOUBLE_RPAREN));
                    i += 2;
                } else {
                    toks.push(RawTok::word(tok::WORD, b")".to_vec()));
                    i += 1;
                }
                continue;
            }
            b'<' => {
                if at(b, i + 1) == b'&' {
                    // <&N / <&- / <&$var : REDIR_DUP_IN with src "0"
                    i += 2;
                    toks.push(RawTok::word(tok::REDIR_DUP_IN, b"0".to_vec()));
                    continue;
                } else if at(b, i + 1) == b'<' {
                    if at(b, i + 2) == b'<' {
                        // here-string: <<< word
                        i += 3;
                        while at(b, i) == b' ' || at(b, i) == b'\t' {
                            i += 1;
                        }
                        let mut hs: Vec<u8> = Vec::new();
                        if at(b, i) == b'\'' || at(b, i) == b'"' {
                            let q = at(b, i);
                            i += 1;
                            while at(b, i) != 0 && at(b, i) != q {
                                hs.push(at(b, i));
                                i += 1;
                            }
                            if at(b, i) == q {
                                i += 1;
                            }
                        } else {
                            while at(b, i) != 0
                                && !is_space(at(b, i))
                                && at(b, i) != b'|'
                                && at(b, i) != b'&'
                                && at(b, i) != b';'
                            {
                                hs.push(at(b, i));
                                i += 1;
                            }
                        }
                        toks.push(RawTok::word(tok::HERESTRING, hs));
                        continue;
                    }
                    // here-doc: << DELIM or <<'DELIM'
                    i += 2;
                    while at(b, i) == b' ' || at(b, i) == b'\t' {
                        i += 1;
                    }
                    let mut noexp = false;
                    if at(b, i) == b'\'' || at(b, i) == b'"' {
                        noexp = true;
                        i += 1;
                    }
                    let delim_start = i;
                    while at(b, i) != 0
                        && !is_space(at(b, i))
                        && at(b, i) != b'\''
                        && at(b, i) != b'"'
                    {
                        i += 1;
                    }
                    let delim = b[delim_start..i.min(b.len())].to_vec();
                    if at(b, i) == b'\'' || at(b, i) == b'"' {
                        i += 1;
                    }
                    let htype = if noexp { tok::HEREDOC_NOEXP } else { tok::HEREDOC };
                    toks.push(RawTok::word(htype, delim));
                    continue;
                } else if at(b, i + 1) == b'(' {
                    // process substitution <(...)
                    let ps_start = i;
                    i += 2;
                    let mut depth = 1;
                    while at(b, i) != 0 && depth > 0 {
                        if at(b, i) == b'(' {
                            depth += 1;
                        } else if at(b, i) == b')' {
                            depth -= 1;
                        }
                        i += 1;
                    }
                    let val = b[ps_start..i.min(b.len())].to_vec();
                    toks.push(RawTok::word(tok::WORD, val));
                    continue;
                } else {
                    toks.push(RawTok::op(tok::REDIR_IN));
                    i += 1;
                    continue;
                }
            }
            b'>' => {
                if at(b, i + 1) == b'>' {
                    toks.push(RawTok::op(tok::REDIR_APP));
                    i += 2;
                    continue;
                } else if at(b, i + 1) == b'&' {
                    i += 2;
                    toks.push(RawTok::word(tok::REDIR_DUP_OUT, b"1".to_vec()));
                    continue;
                } else if at(b, i + 1) == b'(' {
                    let ps_start = i;
                    i += 2;
                    let mut depth = 1;
                    while at(b, i) != 0 && depth > 0 {
                        if at(b, i) == b'(' {
                            depth += 1;
                        } else if at(b, i) == b')' {
                            depth -= 1;
                        }
                        i += 1;
                    }
                    let val = b[ps_start..i.min(b.len())].to_vec();
                    toks.push(RawTok::word(tok::WORD, val));
                    continue;
                } else {
                    toks.push(RawTok::op(tok::REDIR_OUT));
                    i += 1;
                    continue;
                }
            }
            _ => {}
        }

        // ---- $'...' ANSI-C quoting ----
        if c == b'$' && at(b, i + 1) == b'\'' {
            i += 2;
            let mut out: Vec<u8> = Vec::new();
            while at(b, i) != 0 && at(b, i) != b'\'' {
                if at(b, i) == b'\\' && at(b, i + 1) != 0 {
                    i += 1;
                    let e = at(b, i);
                    match e {
                        b'n' => out.push(b'\n'),
                        b't' => out.push(b'\t'),
                        b'r' => out.push(b'\r'),
                        b'a' => out.push(0x07),
                        b'b' => out.push(0x08),
                        b'f' => out.push(0x0c),
                        b'v' => out.push(0x0b),
                        b'e' | b'E' => out.push(0x1b),
                        b'\\' => out.push(b'\\'),
                        b'\'' => out.push(b'\''),
                        b'"' => out.push(b'"'),
                        b'0'..=b'7' => {
                            let mut val = (e - b'0') as u32;
                            if (b'0'..=b'7').contains(&at(b, i + 1)) {
                                i += 1;
                                val = val * 8 + (at(b, i) - b'0') as u32;
                            }
                            if (b'0'..=b'7').contains(&at(b, i + 1)) {
                                i += 1;
                                val = val * 8 + (at(b, i) - b'0') as u32;
                            }
                            out.push(val as u8);
                        }
                        b'x' => {
                            i += 1;
                            let mut val = 0u32;
                            let mut nn = 0;
                            while nn < 2 && at(b, i).is_ascii_hexdigit() {
                                val = val * 16 + hex_val(at(b, i));
                                i += 1;
                                nn += 1;
                            }
                            i -= 1; // matches C `p--`
                            out.push(val as u8);
                        }
                        _ => {
                            out.push(b'\\');
                            out.push(e);
                        }
                    }
                    i += 1; // matches the trailing `p++` after the switch
                } else {
                    out.push(at(b, i));
                    i += 1;
                }
            }
            if at(b, i) == b'\'' {
                i += 1;
            }
            let mut t = RawTok::word(tok::WORD, out);
            t.quoted = 1;
            toks.push(t);
            continue;
        }

        // ---- single-quoted string (value keeps the surrounding quotes) ----
        if c == b'\'' {
            i += 1;
            let start = i;
            while at(b, i) != 0 && at(b, i) != b'\'' {
                i += 1;
            }
            if at(b, i) == b'\'' {
                let content = &b[start..i];
                let mut value = Vec::with_capacity(content.len() + 2);
                value.push(b'\'');
                value.extend_from_slice(content);
                value.push(b'\'');
                let mut t = RawTok::word(tok::WORD, value);
                t.quoted = 1;
                toks.push(t);
                i += 1;
                continue;
            } else {
                return Vec::new(); // unterminated string -> NULL
            }
        }

        // ---- double-quoted string (unescapes \" ; drops surrounding quotes) ----
        if c == b'"' {
            i += 1;
            let mut out: Vec<u8> = Vec::new();
            while at(b, i) != 0 && at(b, i) != b'"' {
                if at(b, i) == b'\\' && at(b, i + 1) == b'"' {
                    out.push(b'"');
                    i += 2;
                } else {
                    out.push(at(b, i));
                    i += 1;
                }
            }
            if at(b, i) == b'"' {
                let mut t = RawTok::word(tok::WORD, out);
                t.quoted = 1;
                toks.push(t);
                i += 1;
                continue;
            } else {
                return Vec::new(); // unterminated string -> NULL
            }
        }

        // ---- fd redirection: N>file, N>>file, N<file, N>&M, {name}>file ... ----
        if is_digit(c) || c == b'{' {
            let fd_start = i;
            let mut fd_name: Vec<u8> = Vec::new();
            let mut fd_num: i64 = -1;
            let mut is_named = false;
            let mut backtrack = false;

            if at(b, i) == b'{' {
                i += 1;
                let mut ni = 0;
                while at(b, i) != 0 && at(b, i) != b'}' && ni < 63 {
                    fd_name.push(at(b, i));
                    i += 1;
                    ni += 1;
                }
                if at(b, i) == b'}' {
                    i += 1;
                    is_named = true;
                } else {
                    i = fd_start;
                    backtrack = true;
                }
            } else {
                let mut ni = 0;
                let mut nbuf: Vec<u8> = Vec::new();
                while is_digit(at(b, i)) && ni < 15 {
                    nbuf.push(at(b, i));
                    i += 1;
                    ni += 1;
                }
                if at(b, i) == b'>' || at(b, i) == b'<' {
                    fd_num = parse_int(&nbuf);
                } else {
                    i = fd_start;
                    backtrack = true;
                }
            }

            if !backtrack {
                if at(b, i) == b'>' {
                    i += 1;
                    let mut is_app = false;
                    if at(b, i) == b'>' {
                        is_app = true;
                        i += 1;
                    }
                    if at(b, i) == b'&' {
                        // N>&M or N>&-
                        i += 1;
                        let mut tgt: Vec<u8> = Vec::new();
                        if at(b, i) == b'-' {
                            tgt.push(b'-');
                            i += 1;
                        } else {
                            let mut ti = 0;
                            while is_digit(at(b, i)) && ti < 15 {
                                tgt.push(at(b, i));
                                i += 1;
                                ti += 1;
                            }
                        }
                        let enc = encode_dup(is_named, &fd_name, fd_num, &tgt);
                        toks.push(RawTok::word(tok::REDIR_DUP_OUT, enc));
                        continue;
                    }
                    let enc = encode_fd(is_named, &fd_name, fd_num);
                    let tt = if is_app { tok::REDIR_FD_APP } else { tok::REDIR_FD_OUT };
                    toks.push(RawTok::word(tt, enc));
                    continue;
                } else if at(b, i) == b'<' {
                    i += 1;
                    if at(b, i) == b'&' {
                        i += 1;
                        let mut tgt: Vec<u8> = Vec::new();
                        if at(b, i) == b'-' {
                            tgt.push(b'-');
                            i += 1;
                        } else {
                            let mut ti = 0;
                            while is_digit(at(b, i)) && ti < 15 {
                                tgt.push(at(b, i));
                                i += 1;
                                ti += 1;
                            }
                        }
                        let enc = encode_dup(is_named, &fd_name, fd_num, &tgt);
                        toks.push(RawTok::word(tok::REDIR_DUP_IN, enc));
                        continue;
                    }
                    let enc = encode_fd(is_named, &fd_name, fd_num);
                    toks.push(RawTok::word(tok::REDIR_FD_IN, enc));
                    continue;
                } else {
                    // named case where char after } is neither > nor <: backtrack
                    i = fd_start;
                }
            }
        }

        // ---- word parsing ----
        let start = i;
        while at(b, i) != 0 {
            let w = at(b, i);
            if is_space(w) {
                break;
            }
            if w == b'|' || w == b'<' || w == b'>' || w == b'&' || w == b';' {
                break;
            }
            if w == b'\'' || w == b'"' {
                break;
            }
            if w == b'$' && at(b, i + 1) == b'(' {
                i += 2;
                let mut depth = 1;
                while at(b, i) != 0 && depth > 0 {
                    if at(b, i) == b'(' {
                        depth += 1;
                    } else if at(b, i) == b')' {
                        depth -= 1;
                    }
                    i += 1;
                }
                continue;
            }
            if w == b'(' || w == b')' {
                break;
            }
            i += 1;
        }

        if i == start {
            if at(b, i) == 0 {
                break 'main;
            }
            i += 1;
            continue;
        }

        // normal word — no quoted suffix
        if i > start && at(b, i - 1) != b'=' && at(b, i) != b'\'' && at(b, i) != b'"' {
            toks.push(RawTok::word(tok::WORD, b[start..i].to_vec()));
            continue;
        }

        // word ends with '=' and next is a quote (var="..."), or empty-prefix quote
        if at(b, i) == b'\'' || at(b, i) == b'"' {
            let quote = at(b, i);
            i += 1;
            let prefix = &b[start..i - 1];
            let prefix_len = prefix.len();
            let mut combined: Vec<u8> = prefix.to_vec();
            while at(b, i) != 0 && at(b, i) != quote {
                if at(b, i) == b'\\' && at(b, i + 1) == quote {
                    combined.push(quote);
                    i += 2;
                } else {
                    combined.push(at(b, i));
                    i += 1;
                }
            }
            if at(b, i) == quote {
                i += 1;
            }
            let is_assignment = combined[..prefix_len].iter().any(|&ch| ch == b'=');
            let mut t = RawTok::word(tok::WORD, combined);
            if !is_assignment {
                t.quoted = 1;
            }
            toks.push(t);
            continue;
        }

        // plain word (e.g. ends with '=' and no following quote)
        toks.push(RawTok::word(tok::WORD, b[start..i].to_vec()));
    }

    toks.push(RawTok::op(tok::EOF));
    toks
}

/// atoi-equivalent for a slice of ASCII digits (already validated as digits).
fn parse_int(digits: &[u8]) -> i64 {
    let mut v: i64 = 0;
    for &d in digits {
        if !d.is_ascii_digit() {
            break;
        }
        v = v * 10 + (d - b'0') as i64;
    }
    v
}

/// Encode the fd field for N>file / {name}>file: "{name}" or "N".
fn encode_fd(is_named: bool, fd_name: &[u8], fd_num: i64) -> Vec<u8> {
    if is_named {
        let mut v = Vec::new();
        v.push(b'{');
        v.extend_from_slice(fd_name);
        v.push(b'}');
        v
    } else {
        fd_num.to_string().into_bytes()
    }
}

/// Encode the dup field for N>&M / {name}>&M: "{name}&tgt" or "N&tgt".
fn encode_dup(is_named: bool, fd_name: &[u8], fd_num: i64, tgt: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    if is_named {
        v.push(b'{');
        v.extend_from_slice(fd_name);
        v.push(b'}');
    } else {
        v.extend_from_slice(fd_num.to_string().as_bytes());
    }
    v.push(b'&');
    v.extend_from_slice(tgt);
    v
}

// ---- FFI entry points ----

#[no_mangle]
pub unsafe extern "C" fn lex_rs(input: *const c_char, ntokens: *mut c_int) -> *mut Token {
    // SAFETY: `input` is NULL or a valid NUL-terminated C string owned by the
    // caller for the duration of the call; `ntokens` is a valid writable int.
    let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if input.is_null() {
            return ptr::null_mut();
        }
        let len = libc::strlen(input);
        let bytes = core::slice::from_raw_parts(input as *const u8, len);
        let toks = lex_internal(bytes);
        if toks.is_empty() {
            // unterminated quote: C `lex` returns NULL
            return ptr::null_mut();
        }
        let count = toks.len();
        let arr = materialize(toks);
        if arr.is_null() {
            return ptr::null_mut();
        }
        if !ntokens.is_null() {
            *ntokens = count as c_int;
        }
        arr
    }));
    res.unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub unsafe extern "C" fn tokens_free_rs(toks: *mut Token, n: c_int) {
    // SAFETY: `toks` is NULL or a Token array of length `n` produced by lex_rs
    // (every `value` and the array itself on the C heap).
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if toks.is_null() {
            return;
        }
        for k in 0..n.max(0) {
            let t = toks.add(k as usize);
            if !(*t).value.is_null() {
                libc::free((*t).value as *mut c_void);
            }
        }
        libc::free(toks as *mut c_void);
    }));
}
