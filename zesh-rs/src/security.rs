// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// security.rs — Rust reimplementation of src/security.c.
//
// Dangerous-command detection (SEC_WARN / SEC_BLOCK) and the append-only audit
// log. The danger rule table and matching logic mirror the C version exactly,
// including substring vs. prefix matching and "keep the highest severity"
// selection. Reason strings are returned as pointers to static NUL-terminated
// data, exactly like the C string literals.

use libc::{c_char, c_int, c_void};
use core::ptr;

use crate::ffi::config_ptr;

// SecurityLevel (include/security.h): SEC_OK=0, SEC_WARN=1, SEC_BLOCK=2.
const SEC_OK: c_int = 0;
const SEC_WARN: c_int = 1;
const SEC_BLOCK: c_int = 2;

struct DangerRule {
    pattern: &'static [u8],
    is_prefix: bool, // C is_regex == 1 means "prefix match"
    level: c_int,
    reason: &'static [u8], // NUL-terminated for returning as *const c_char
}

macro_rules! rule {
    ($pat:literal, $prefix:expr, $lvl:expr, $reason:literal) => {
        DangerRule {
            pattern: $pat,
            is_prefix: $prefix,
            level: $lvl,
            reason: concat!($reason, "\0").as_bytes(),
        }
    };
}

static RULES: &[DangerRule] = &[
    // Disk/filesystem destruction
    rule!(b"rm -rf /", false, SEC_WARN, "Entire filesystem will be deleted!"),
    rule!(b"rm -rf /*", false, SEC_WARN, "Entire filesystem will be deleted!"),
    rule!(b"rm -fr /", false, SEC_WARN, "Entire filesystem will be deleted!"),
    rule!(b"dd if=/dev/zero", false, SEC_WARN, "Disk will be completely overwritten with zeros!"),
    rule!(b"dd if=/dev/urandom", false, SEC_WARN, "Disk will be overwritten with random data!"),
    rule!(b"mkfs.", true, SEC_WARN, "Filesystem will be formatted!"),
    rule!(b"fdisk", true, SEC_WARN, "Disk partitioning tool"),
    rule!(b"parted", true, SEC_WARN, "Disk partitioning tool"),
    rule!(b"wipefs", true, SEC_WARN, "Disk signatures will be erased!"),
    rule!(b"> /dev/sd", false, SEC_WARN, "Direct write to disk device!"),
    // System-wide changes
    rule!(b"chmod -R 777 /", false, SEC_WARN, "All system permissions will be altered!"),
    rule!(b"chown -R", false, SEC_WARN, "Ownership is being modified recursively!"),
    rule!(b"chmod 777", false, SEC_WARN, "Insecure permissions (777)!"),
    // Fork bomb
    rule!(b":(){ :|:& };:", false, SEC_BLOCK, "Fork bomb detected!"),
    rule!(b":(){ :|: &};:", false, SEC_BLOCK, "Fork bomb detected!"),
    // Sensitive file operations
    rule!(b"shred /dev/", false, SEC_WARN, "Device data will be permanently erased!"),
    rule!(b"rm -rf ~", false, SEC_WARN, "Home directory will be deleted!"),
    rule!(b"rm -rf $HOME", false, SEC_WARN, "Home directory will be deleted!"),
    // Network danger
    rule!(b"nc -e /bin/sh", false, SEC_WARN, "Reverse shell detected!"),
    rule!(b"nc -e /bin/bash", false, SEC_WARN, "Reverse shell detected!"),
    rule!(b"bash -i >& /dev/tcp", false, SEC_WARN, "Reverse shell detected!"),
    // History/audit tampering
    rule!(b"rm ~/.zesh_history", false, SEC_WARN, "Shell history will be deleted!"),
    rule!(b"> ~/.zesh_history", false, SEC_WARN, "Shell history will be cleared!"),
];

/// SAFETY: `p` is NULL or a valid NUL-terminated C string.
unsafe fn cstr_bytes<'a>(p: *const c_char) -> Option<&'a [u8]> {
    if p.is_null() {
        return None;
    }
    let len = libc::strlen(p);
    Some(core::slice::from_raw_parts(p as *const u8, len))
}

fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    if needle.len() > haystack.len() {
        return false;
    }
    haystack.windows(needle.len()).any(|w| w == needle)
}

#[no_mangle]
pub unsafe extern "C" fn security_check_rs(
    cmdline: *const c_char,
    reason: *mut *const c_char,
) -> c_int {
    let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        // C: if (!cmdline || !g_config.security_warn) return SEC_OK;
        let bytes = match cstr_bytes(cmdline) {
            Some(b) => b,
            None => return SEC_OK,
        };
        if (*config_ptr()).security_warn == 0 {
            return SEC_OK;
        }

        let mut highest = SEC_OK;
        let mut highest_reason: *const c_char = ptr::null();

        for r in RULES {
            let matched = if r.is_prefix {
                let mut p = bytes;
                while !p.is_empty() && p[0] == b' ' {
                    p = &p[1..];
                }
                p.starts_with(r.pattern)
            } else {
                contains(bytes, r.pattern)
            };
            if matched && r.level > highest {
                highest = r.level;
                highest_reason = r.reason.as_ptr() as *const c_char;
            }
        }

        if !reason.is_null() {
            *reason = highest_reason;
        }
        highest
    }));
    res.unwrap_or(SEC_OK)
}

#[no_mangle]
pub unsafe extern "C" fn security_audit_rs(cmdline: *const c_char) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        // C: if (!g_config.security_audit || !cmdline) return;
        let cmd = match cstr_bytes(cmdline) {
            Some(b) => b,
            None => return,
        };
        if (*config_ptr()).security_audit == 0 {
            return;
        }

        // Expand a leading "~/" in the log path against $HOME.
        let log_field_ptr =
            core::ptr::addr_of!((*config_ptr()).security_audit_log) as *const c_char;
        let log_len = libc::strlen(log_field_ptr);
        let log_bytes = core::slice::from_raw_parts(log_field_ptr as *const u8, log_len);

        let mut path: Vec<u8> = Vec::new();
        let home = libc::getenv(b"HOME\0".as_ptr() as *const c_char);
        if !home.is_null() && log_bytes.starts_with(b"~/") {
            let hlen = libc::strlen(home);
            path.extend_from_slice(core::slice::from_raw_parts(home as *const u8, hlen));
            path.push(b'/');
            path.extend_from_slice(&log_bytes[2..]);
        } else {
            // C strncpy into a 512 buffer (truncates); Vec just copies it all.
            path.extend_from_slice(&log_bytes[..log_bytes.len().min(511)]);
        }
        path.push(0);

        let f = libc::fopen(path.as_ptr() as *const c_char, b"a\0".as_ptr() as *const c_char);
        if f.is_null() {
            return;
        }

        // Timestamp: "%Y-%m-%d %H:%M:%S" via localtime/strftime, like the C side.
        let mut t: libc::time_t = libc::time(ptr::null_mut());
        let tm = libc::localtime(&mut t as *const libc::time_t);
        let mut timebuf = [0u8; 32];
        let mut line: Vec<u8> = Vec::new();
        if !tm.is_null() {
            let n = libc::strftime(
                timebuf.as_mut_ptr() as *mut c_char,
                timebuf.len(),
                b"%Y-%m-%d %H:%M:%S\0".as_ptr() as *const c_char,
                tm,
            );
            line.push(b'[');
            line.extend_from_slice(&timebuf[..n]);
            line.extend_from_slice(b"] ");
        }
        line.extend_from_slice(cmd);
        line.push(b'\n');

        libc::fwrite(line.as_ptr() as *const c_void, 1, line.len(), f);
        libc::fclose(f);
    }));
}

#[no_mangle]
pub extern "C" fn security_init_rs() {
    // C: nothing for now — config already loaded.
}
