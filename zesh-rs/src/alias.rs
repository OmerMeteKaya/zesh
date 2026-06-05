// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// alias.rs — Rust reimplementation of src/alias.c.
//
// The C version is a plain fixed array (MAX_ALIASES) with a linear scan. This
// port keeps an *ordered* Vec behind a Mutex so it is thread-safe by
// construction while preserving the observable behaviour the C code has:
//   * alias_list prints entries in insertion order;
//   * alias_add replaces an existing alias in place, else appends (capped at
//     MAX_ALIASES, printing the same diagnostic to stderr on overflow);
//   * alias_expand returns a pointer that stays valid until the next
//     alias_expand call — callers (main.c, highlight.c) copy it synchronously
//     and never free it, exactly like the C side returning &table[i].value.

use libc::{c_char, c_void};
use core::ptr;
use std::sync::Mutex;
use std::sync::atomic::{AtomicPtr, Ordering};

const MAX_ALIASES: usize = 128;

struct Entry {
    name: Vec<u8>,
    value: Vec<u8>,
}

static ALIASES: Mutex<Vec<Entry>> = Mutex::new(Vec::new());

// Holds the C string most recently handed back by alias_expand. Freed and
// replaced on each call, so exactly one copy is ever outstanding (leak-free)
// and the pointer is valid until the next alias_expand — matching how the C
// callers use it.
static LAST_EXPAND: AtomicPtr<c_char> = AtomicPtr::new(ptr::null_mut());

/// SAFETY: `p` is NULL or a valid NUL-terminated C string. Returns None for NULL.
unsafe fn cstr_bytes(p: *const c_char) -> Option<Vec<u8>> {
    if p.is_null() {
        return None;
    }
    let len = libc::strlen(p);
    Some(core::slice::from_raw_parts(p as *const u8, len).to_vec())
}

/// Allocate a NUL-terminated C string on the C heap from raw bytes.
unsafe fn c_strdup_bytes(bytes: &[u8]) -> *mut c_char {
    let p = libc::malloc(bytes.len() + 1) as *mut u8;
    if p.is_null() {
        return ptr::null_mut();
    }
    ptr::copy_nonoverlapping(bytes.as_ptr(), p, bytes.len());
    *p.add(bytes.len()) = 0;
    p as *mut c_char
}

fn guard<R>(f: impl FnOnce(&mut Vec<Entry>) -> R) -> R {
    let mut t = ALIASES.lock().unwrap_or_else(|e| e.into_inner());
    f(&mut t)
}

#[no_mangle]
pub extern "C" fn alias_init_rs() {
    guard(|t| t.clear());
}

#[no_mangle]
pub unsafe extern "C" fn alias_add_rs(name: *const c_char, value: *const c_char) {
    let _ = std::panic::catch_unwind(|| {
        let (name, value) = match (cstr_bytes(name), cstr_bytes(value)) {
            (Some(n), Some(v)) => (n, v),
            _ => return, // C: if (!name || !value) return;
        };
        guard(|t| {
            if let Some(e) = t.iter_mut().find(|e| e.name == name) {
                e.value = value;
                return;
            }
            if t.len() >= MAX_ALIASES {
                let msg = b"alias: maximum number of aliases reached\n";
                // SAFETY: writing a static byte string to stderr.
                unsafe { libc::write(2, msg.as_ptr() as *const c_void, msg.len()); }
                return;
            }
            t.push(Entry { name, value });
        });
    });
}

#[no_mangle]
pub unsafe extern "C" fn alias_remove_rs(name: *const c_char) {
    let _ = std::panic::catch_unwind(|| {
        let name = match cstr_bytes(name) {
            Some(n) => n,
            None => return,
        };
        guard(|t| {
            if let Some(pos) = t.iter().position(|e| e.name == name) {
                t.remove(pos);
            }
        });
    });
}

#[no_mangle]
pub unsafe extern "C" fn alias_expand_rs(name: *const c_char) -> *mut c_char {
    let res = std::panic::catch_unwind(|| {
        let name = match cstr_bytes(name) {
            Some(n) => n,
            None => return ptr::null_mut(),
        };
        let found = guard(|t| t.iter().find(|e| e.name == name).map(|e| e.value.clone()));
        match found {
            Some(v) => {
                // SAFETY: produce a fresh C string, free the previous one.
                let s = unsafe { c_strdup_bytes(&v) };
                let old = LAST_EXPAND.swap(s, Ordering::SeqCst);
                if !old.is_null() {
                    unsafe { libc::free(old as *mut c_void) };
                }
                s
            }
            None => ptr::null_mut(),
        }
    });
    res.unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn alias_list_rs() {
    let _ = std::panic::catch_unwind(|| {
        let lines: Vec<Vec<u8>> = guard(|t| {
            t.iter()
                .map(|e| {
                    let mut line = b"alias ".to_vec();
                    line.extend_from_slice(&e.name);
                    line.extend_from_slice(b"='");
                    line.extend_from_slice(&e.value);
                    line.extend_from_slice(b"'\n");
                    line
                })
                .collect()
        });
        for line in lines {
            // SAFETY: writing owned bytes to stdout.
            unsafe { libc::write(1, line.as_ptr() as *const c_void, line.len()); }
        }
    });
}

#[no_mangle]
pub extern "C" fn alias_free_rs() {
    let _ = std::panic::catch_unwind(|| {
        guard(|t| t.clear());
    });
}

#[no_mangle]
pub unsafe extern "C" fn alias_each_rs(
    cb: Option<unsafe extern "C" fn(*const c_char, *const c_char, *mut c_void)>,
    ud: *mut c_void,
) {
    let cb = match cb {
        Some(f) => f,
        None => return,
    };
    // Snapshot under lock, then invoke callbacks without holding it (the C
    // callback must not be able to deadlock on the alias table).
    let snapshot: Vec<(Vec<u8>, Vec<u8>)> =
        guard(|t| t.iter().map(|e| (e.name.clone(), e.value.clone())).collect());
    for (name, value) in snapshot {
        let mut n = name;
        n.push(0);
        let mut v = value;
        v.push(0);
        // SAFETY: n/v are NUL-terminated and live for the duration of the call.
        cb(n.as_ptr() as *const c_char, v.as_ptr() as *const c_char, ud);
    }
}
