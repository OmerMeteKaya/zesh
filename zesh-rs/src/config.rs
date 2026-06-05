// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// config.rs — Rust reimplementation of src/config.c (config_load/config_save).
//
// The storage (`g_config`) stays C-owned (defined in config.c, always); this
// module only parses ~/.zesh/config into it and serializes it back out. The
// key set, key=value splitting and quote stripping match the C version exactly,
// so valid config files produce a byte-identical g_config.
//
// NEW (Rust-only) — corrupt-proof schema validation:
//   Every recognized key is validated against a typed schema (Bool / Int range
//   / Color). On an invalid value the C side would silently apply garbage
//   (atoi("x")==0, parse_bool("x")==false, colors copied verbatim); this port
//   instead KEEPS the default already in g_config, logs a warning to stderr,
//   and continues — never leaving config half-applied. Valid values behave
//   exactly like C, so test/behaviour parity holds for well-formed configs.

use libc::{c_char, c_int, c_void};

use crate::ffi::{config_ptr, config_ptr_mut, ShellConfig};

// ---- value kinds + schema ----

#[derive(Clone, Copy)]
enum Kind {
    Bool,
    Int { min: i64, max: i64 },
    Color,
}

struct ConfigSchema {
    key: &'static str,
    kind: Kind,
    validate: fn(&[u8]) -> bool,
}

fn validate_bool(v: &[u8]) -> bool {
    matches!(
        v,
        b"true" | b"false" | b"1" | b"0" | b"yes" | b"no" | b"on" | b"off"
    )
}

fn validate_int_generic(v: &[u8]) -> bool {
    if v.is_empty() {
        return false;
    }
    let (sign, rest) = if v[0] == b'-' || v[0] == b'+' {
        (true, &v[1..])
    } else {
        (false, v)
    };
    if rest.is_empty() {
        return false;
    }
    let _ = sign;
    rest.iter().all(|c| c.is_ascii_digit())
}

const NAMED_COLORS: &[&[u8]] = &[
    b"black", b"red", b"green", b"yellow", b"blue", b"magenta", b"cyan",
    b"white", b"default", b"gray", b"grey", b"dim", b"bold", b"bright",
    b"brightred", b"brightgreen", b"brightyellow", b"brightblue",
    b"brightmagenta", b"brightcyan", b"brightwhite", b"none", b"reset",
];

fn validate_color(v: &[u8]) -> bool {
    if v.is_empty() {
        return false;
    }
    // #RRGGBB
    if v[0] == b'#' {
        return v.len() == 7 && v[1..].iter().all(|c| c.is_ascii_hexdigit());
    }
    // Raw ANSI escape sequence, or a bare numeric SGR code (e.g. "31").
    if v.contains(&0x1b) || v.iter().all(|c| c.is_ascii_digit()) {
        return true;
    }
    // Named color (case-insensitive).
    let lower: Vec<u8> = v.iter().map(|c| c.to_ascii_lowercase()).collect();
    NAMED_COLORS.iter().any(|n| *n == lower.as_slice())
}

static SCHEMA: &[ConfigSchema] = &[
    ConfigSchema { key: "prompt_show_time", kind: Kind::Bool, validate: validate_bool },
    ConfigSchema { key: "prompt_show_user", kind: Kind::Bool, validate: validate_bool },
    ConfigSchema { key: "prompt_color_ok", kind: Kind::Color, validate: validate_color },
    ConfigSchema { key: "prompt_color_err", kind: Kind::Color, validate: validate_color },
    ConfigSchema { key: "history_max", kind: Kind::Int { min: 0, max: 100_000_000 }, validate: validate_int_generic },
    ConfigSchema { key: "history_dedup", kind: Kind::Bool, validate: validate_bool },
    ConfigSchema { key: "panel_max_rows", kind: Kind::Int { min: 0, max: 10_000 }, validate: validate_int_generic },
    ConfigSchema { key: "panel_max_items", kind: Kind::Int { min: 0, max: 1_000_000 }, validate: validate_int_generic },
    ConfigSchema { key: "panel_enabled", kind: Kind::Bool, validate: validate_bool },
    ConfigSchema { key: "completion_enabled", kind: Kind::Bool, validate: validate_bool },
    ConfigSchema { key: "suggestion_enabled", kind: Kind::Bool, validate: validate_bool },
    ConfigSchema { key: "highlight_enabled", kind: Kind::Bool, validate: validate_bool },
    ConfigSchema { key: "hl_color_keyword", kind: Kind::Color, validate: validate_color },
    ConfigSchema { key: "hl_color_string", kind: Kind::Color, validate: validate_color },
    ConfigSchema { key: "hl_color_variable", kind: Kind::Color, validate: validate_color },
    ConfigSchema { key: "hl_color_comment", kind: Kind::Color, validate: validate_color },
    ConfigSchema { key: "hl_color_operator", kind: Kind::Color, validate: validate_color },
    ConfigSchema { key: "hl_color_cmd_ok", kind: Kind::Color, validate: validate_color },
    ConfigSchema { key: "hl_color_cmd_err", kind: Kind::Color, validate: validate_color },
    ConfigSchema { key: "hl_color_path", kind: Kind::Color, validate: validate_color },
    ConfigSchema { key: "hl_color_flag", kind: Kind::Color, validate: validate_color },
];

// ---- parsing helpers (match config.c semantics) ----

fn is_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

/// strip() — trim leading and trailing whitespace.
fn strip(s: &[u8]) -> &[u8] {
    let mut start = 0;
    while start < s.len() && is_space(s[start]) {
        start += 1;
    }
    let mut end = s.len();
    while end > start && is_space(s[end - 1]) {
        end -= 1;
    }
    &s[start..end]
}

/// parse_bool() — true for true/1/yes/on (matches config.c).
fn parse_bool(v: &[u8]) -> c_int {
    if v == b"true" || v == b"1" || v == b"yes" || v == b"on" {
        1
    } else {
        0
    }
}

/// atoi() — leading optional sign then digits, stop at first non-digit.
fn atoi(v: &[u8]) -> c_int {
    let mut i = 0;
    let mut neg = false;
    if i < v.len() && (v[i] == b'-' || v[i] == b'+') {
        neg = v[i] == b'-';
        i += 1;
    }
    let mut n: i64 = 0;
    while i < v.len() && v[i].is_ascii_digit() {
        n = n * 10 + (v[i] - b'0') as i64;
        i += 1;
    }
    if neg {
        n = -n;
    }
    n as c_int
}

/// strncpy(dst, src, 15) into a [c_char;16] field: copies up to 15 bytes,
/// NUL-padding the remainder of those 15 (index 15 is left untouched, exactly
/// like the C `sizeof(field)-1` strncpy).
unsafe fn strncpy_field(field: *mut c_char, src: &[u8]) {
    let n = 15usize;
    for i in 0..n {
        let b = if i < src.len() { src[i] as c_char } else { 0 };
        *field.add(i) = b;
    }
}

unsafe fn warn(key: &[u8], val: &[u8]) {
    let mut msg = b"zesh: config: invalid value '".to_vec();
    msg.extend_from_slice(val);
    msg.extend_from_slice(b"' for '");
    msg.extend_from_slice(key);
    msg.extend_from_slice(b"', using default\n");
    libc::write(2, msg.as_ptr() as *const c_void, msg.len());
}

/// Apply one validated key=value into g_config. Unknown keys are ignored
/// (matching the C else-chain that silently drops them).
unsafe fn apply(key: &[u8], val: &[u8]) {
    let key_str = match core::str::from_utf8(key) {
        Ok(s) => s,
        Err(_) => return,
    };
    let schema = match SCHEMA.iter().find(|s| s.key == key_str) {
        Some(s) => s,
        None => return,
    };
    if !(schema.validate)(val) {
        warn(key, val);
        return;
    }
    // Range check for ints (validator only checks digit-shape).
    if let Kind::Int { min, max } = schema.kind {
        let n = atoi(val) as i64;
        if n < min || n > max {
            warn(key, val);
            return;
        }
    }

    let cfg: *mut ShellConfig = config_ptr_mut();
    match key_str {
        "prompt_show_time" => (*cfg).prompt_show_time = parse_bool(val),
        "prompt_show_user" => (*cfg).prompt_show_user = parse_bool(val),
        "prompt_color_ok" => strncpy_field(core::ptr::addr_of_mut!((*cfg).prompt_color_ok) as *mut c_char, val),
        "prompt_color_err" => strncpy_field(core::ptr::addr_of_mut!((*cfg).prompt_color_err) as *mut c_char, val),
        "history_max" => (*cfg).history_max = atoi(val),
        "history_dedup" => (*cfg).history_dedup = parse_bool(val),
        "panel_max_rows" => (*cfg).panel_max_rows = atoi(val),
        "panel_max_items" => (*cfg).panel_max_items = atoi(val),
        "panel_enabled" => (*cfg).panel_enabled = parse_bool(val),
        "completion_enabled" => (*cfg).completion_enabled = parse_bool(val),
        "suggestion_enabled" => (*cfg).suggestion_enabled = parse_bool(val),
        "highlight_enabled" => (*cfg).highlight_enabled = parse_bool(val),
        "hl_color_keyword" => strncpy_field(core::ptr::addr_of_mut!((*cfg).hl_color_keyword) as *mut c_char, val),
        "hl_color_string" => strncpy_field(core::ptr::addr_of_mut!((*cfg).hl_color_string) as *mut c_char, val),
        "hl_color_variable" => strncpy_field(core::ptr::addr_of_mut!((*cfg).hl_color_variable) as *mut c_char, val),
        "hl_color_comment" => strncpy_field(core::ptr::addr_of_mut!((*cfg).hl_color_comment) as *mut c_char, val),
        "hl_color_operator" => strncpy_field(core::ptr::addr_of_mut!((*cfg).hl_color_operator) as *mut c_char, val),
        "hl_color_cmd_ok" => strncpy_field(core::ptr::addr_of_mut!((*cfg).hl_color_cmd_ok) as *mut c_char, val),
        "hl_color_cmd_err" => strncpy_field(core::ptr::addr_of_mut!((*cfg).hl_color_cmd_err) as *mut c_char, val),
        "hl_color_path" => strncpy_field(core::ptr::addr_of_mut!((*cfg).hl_color_path) as *mut c_char, val),
        "hl_color_flag" => strncpy_field(core::ptr::addr_of_mut!((*cfg).hl_color_flag) as *mut c_char, val),
        _ => {}
    }
}

/// Parse one line, mirroring config.c: strip, skip empty/comment, split on the
/// first '=', strip both sides, strip surrounding matching quotes from value.
unsafe fn parse_line(line: &[u8]) {
    let p = strip(line);
    if p.is_empty() || p[0] == b'#' {
        return;
    }
    let eq = match p.iter().position(|&c| c == b'=') {
        Some(i) => i,
        None => return,
    };
    let key = strip(&p[..eq]);
    let mut val = strip(&p[eq + 1..]);

    // Strip surrounding quotes: leading quote removed unconditionally, trailing
    // only if it matches (faithful to config.c).
    if !val.is_empty() && (val[0] == b'"' || val[0] == b'\'') {
        let q = val[0];
        val = &val[1..];
        if !val.is_empty() && val[val.len() - 1] == q {
            val = &val[..val.len() - 1];
        }
    }

    apply(key, val);
}

// ---- byte helper for serialization ----

unsafe fn field_bytes(field: *const c_char) -> Vec<u8> {
    let len = libc::strlen(field);
    core::slice::from_raw_parts(field as *const u8, len).to_vec()
}

// ---- FFI entry points ----

#[no_mangle]
pub unsafe extern "C" fn config_load_rs(path: *const c_char) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if path.is_null() {
            return;
        }
        let f = libc::fopen(path, b"r\0".as_ptr() as *const c_char);
        if f.is_null() {
            return; // file may not exist — use defaults
        }
        // Read whole file, then split into lines (config.c reads with fgets and
        // a 512 buffer; for well-formed configs this is equivalent).
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

        for line in data.split(|&c| c == b'\n') {
            parse_line(line);
        }
    }));
}

#[no_mangle]
pub unsafe extern "C" fn config_save_rs(path: *const c_char) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if path.is_null() {
            return;
        }
        let f = libc::fopen(path, b"w\0".as_ptr() as *const c_char);
        if f.is_null() {
            return;
        }
        let cfg = config_ptr();
        let b = |v: c_int| -> &'static [u8] { if v != 0 { b"true" } else { b"false" } };

        let mut out: Vec<u8> = Vec::new();
        out.extend_from_slice(b"# zesh configuration\n");
        out.extend_from_slice(b"# Generated automatically \xe2\x80\x94 edit as needed\n\n");
        out.extend_from_slice(b"# Prompt\n");
        out.extend_from_slice(b"prompt_show_time=");
        out.extend_from_slice(b((*cfg).prompt_show_time));
        out.push(b'\n');
        out.extend_from_slice(b"prompt_show_user=");
        out.extend_from_slice(b((*cfg).prompt_show_user));
        out.push(b'\n');
        out.extend_from_slice(b"prompt_color_ok=");
        out.extend_from_slice(&field_bytes(core::ptr::addr_of!((*cfg).prompt_color_ok) as *const c_char));
        out.push(b'\n');
        out.extend_from_slice(b"prompt_color_err=");
        out.extend_from_slice(&field_bytes(core::ptr::addr_of!((*cfg).prompt_color_err) as *const c_char));
        out.push(b'\n');

        out.extend_from_slice(b"\n# History\n");
        out.extend_from_slice(format!("history_max={}\n", (*cfg).history_max).as_bytes());
        out.extend_from_slice(b"history_dedup=");
        out.extend_from_slice(b((*cfg).history_dedup));
        out.push(b'\n');

        out.extend_from_slice(b"\n# Panel\n");
        out.extend_from_slice(format!("panel_max_rows={}\n", (*cfg).panel_max_rows).as_bytes());
        out.extend_from_slice(format!("panel_max_items={}\n", (*cfg).panel_max_items).as_bytes());
        out.extend_from_slice(b"panel_enabled=");
        out.extend_from_slice(b((*cfg).panel_enabled));
        out.push(b'\n');

        out.extend_from_slice(b"\n# Completion\n");
        out.extend_from_slice(b"completion_enabled=");
        out.extend_from_slice(b((*cfg).completion_enabled));
        out.push(b'\n');
        out.extend_from_slice(b"suggestion_enabled=");
        out.extend_from_slice(b((*cfg).suggestion_enabled));
        out.push(b'\n');

        out.extend_from_slice(b"\n# Syntax highlight\n");
        out.extend_from_slice(b"highlight_enabled=");
        out.extend_from_slice(b((*cfg).highlight_enabled));
        out.push(b'\n');

        libc::fwrite(out.as_ptr() as *const c_void, 1, out.len(), f);
        libc::fclose(f);
    }));
}
