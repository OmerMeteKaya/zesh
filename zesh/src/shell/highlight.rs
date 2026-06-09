// Syntax highlighting for the interactive prompt.
// Ported from the C highlight.o implementation (source lost; re-derived from spec).

use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};

use crate::shell::config::config_load;

const RESET:     &str = "\x1b[0m";
const KEYWORD_C: &str = "\x1b[35m";   // magenta — keywords
const CMD_OK_C:  &str = "\x1b[32m";   // green   — found command
const CMD_ERR_C: &str = "\x1b[31m";   // red     — not found command
const VAR_C:     &str = "\x1b[33m";   // yellow  — variables
const STR_C:     &str = "\x1b[32m";   // green   — strings
const COMMENT_C: &str = "\x1b[2;37m"; // dim gray — comments
const FLAG_C:    &str = "\x1b[33m";   // yellow  — flags (-f / --flag)
const PATH_C:    &str = "\x1b[36m";   // cyan    — paths
const ERR_C:     &str = "\x1b[31m";   // red     — unclosed quote / var
const NEG_KW_C:  &str = "\x1b[1;31m"; // bold red — extra fi/done/esac

static CMD_CACHE: OnceLock<Mutex<HashMap<String, bool>>> = OnceLock::new();

const BUILTINS: &[&str] = &[
    "cd", "exit", "export", "pwd", "echo", "alias", "unalias", "source",
    ".", "jobs", "fg", "bg", "set", "unset", "readonly", "local",
    "declare", "read", "printf", "trap", "eval", "type", "hash", "wait",
    "true", "false", "test", "[", "return", "shift", "getopts", "exec",
    "umask", "ulimit", "kill", "break", "continue", "let", "builtin",
    "command", ":", "times",
];

// Opening keywords: increase depth, next token is command/condition
const OPENS: &[&str] = &["if", "while", "for", "case", "until"];
// Closing keywords: decrease depth
const CLOSES: &[&str] = &["fi", "done", "esac"];
// Body starters: no depth change, next token is a command
const CMD_NEXT: &[&str] = &["then", "do", "else", "elif"];

fn command_exists(cmd: &str) -> bool {
    if cmd.is_empty() { return false; }
    if BUILTINS.contains(&cmd) { return true; }

    let cache = CMD_CACHE.get_or_init(|| Mutex::new(HashMap::new()));
    {
        if let Ok(guard) = cache.lock() {
            if let Some(&exists) = guard.get(cmd) {
                return exists;
            }
        }
    }

    let exists = check_in_path(cmd);
    if let Ok(mut guard) = cache.lock() {
        guard.insert(cmd.to_string(), exists);
    }
    exists
}

fn check_in_path(cmd: &str) -> bool {
    if cmd.starts_with('/') || cmd.starts_with("./") || cmd.starts_with("../") {
        if let Ok(s) = std::ffi::CString::new(cmd) {
            // SAFETY: access() is a read-only POSIX syscall; pointer is valid for the duration.
            return unsafe { libc::access(s.as_ptr(), libc::X_OK) } == 0;
        }
        return false;
    }

    let path_env = match std::env::var("PATH") {
        Ok(p) => p,
        Err(_) => return false,
    };

    for dir in path_env.split(':') {
        if dir.is_empty() { continue; }
        let full = format!("{}/{}", dir, cmd);
        if let Ok(s) = std::ffi::CString::new(full.as_bytes()) {
            // SAFETY: access() is a read-only POSIX syscall; pointer is valid for the duration.
            if unsafe { libc::access(s.as_ptr(), libc::X_OK) } == 0 {
                return true;
            }
        }
    }
    false
}

// Returns true if `b` ends a word token (is a delimiter).
#[inline]
fn is_word_end(b: u8) -> bool {
    matches!(b,
        b' ' | b'\t' | b'|' | b'>' | b'<' | b'&' | b';' |
        b'\'' | b'"' | b'$' | b'(' | b')' | b'{' | b'}')
}

// Append bytes to String, replacing invalid UTF-8 with replacement chars.
fn push_lossy(out: &mut String, bytes: &[u8]) {
    match std::str::from_utf8(bytes) {
        Ok(s) => out.push_str(s),
        Err(_) => out.push_str(&String::from_utf8_lossy(bytes)),
    }
}

pub fn highlight(line: &str) -> String {
    let cfg = config_load();
    if !cfg.features.highlighting {
        return line.to_string();
    }
    if line.is_empty() {
        return String::new();
    }

    let bytes = line.as_bytes();
    let n = bytes.len();
    let mut out = String::with_capacity(n * 4);
    let mut i: usize = 0;
    let mut at_cmd_pos = true;
    let mut depth: i32 = 0;

    while i < n {
        let c = bytes[i];

        // ── Whitespace ───────────────────────────────────────────────
        if c == b' ' || c == b'\t' {
            out.push(c as char);
            i += 1;
            continue;
        }

        // ── Comment: # (only at token boundary — we arrive here after
        //   whitespace/operators, so this is always a token start) ───
        if c == b'#' {
            out.push_str(COMMENT_C);
            push_lossy(&mut out, &bytes[i..]);
            out.push_str(RESET);
            break;
        }

        // ── Single-quoted string ──────────────────────────────────────
        if c == b'\'' {
            let start = i;
            i += 1;
            while i < n && bytes[i] != b'\'' { i += 1; }
            if i < n {
                out.push_str(STR_C);
                push_lossy(&mut out, &bytes[start..=i]);
                out.push_str(RESET);
                i += 1;
            } else {
                out.push_str(ERR_C);
                push_lossy(&mut out, &bytes[start..]);
                out.push_str(RESET);
            }
            at_cmd_pos = false;
            continue;
        }

        // ── Double-quoted string ──────────────────────────────────────
        if c == b'"' {
            let start = i;
            i += 1;
            while i < n {
                if bytes[i] == b'\\' {
                    i += 1;
                    if i < n { i += 1; }
                    continue;
                }
                if bytes[i] == b'"' { break; }
                i += 1;
            }
            if i < n {
                out.push_str(STR_C);
                push_lossy(&mut out, &bytes[start..=i]);
                out.push_str(RESET);
                i += 1;
            } else {
                out.push_str(ERR_C);
                push_lossy(&mut out, &bytes[start..]);
                out.push_str(RESET);
            }
            at_cmd_pos = false;
            continue;
        }

        // ── Variable: $VAR  ${VAR}  $(...) ───────────────────────────
        if c == b'$' {
            let start = i;
            if i + 1 < n && bytes[i + 1] == b'{' {
                i += 2;
                while i < n && bytes[i] != b'}' { i += 1; }
                if i < n {
                    out.push_str(VAR_C);
                    push_lossy(&mut out, &bytes[start..=i]);
                    out.push_str(RESET);
                    i += 1;
                } else {
                    out.push_str(ERR_C);
                    push_lossy(&mut out, &bytes[start..]);
                    out.push_str(RESET);
                }
            } else if i + 1 < n && bytes[i + 1] == b'(' {
                i += 2;
                let mut pd: i32 = 1;
                while i < n && pd > 0 {
                    match bytes[i] {
                        b'(' => pd += 1,
                        b')' => pd -= 1,
                        _ => {}
                    }
                    i += 1;
                }
                if pd == 0 {
                    out.push_str(VAR_C);
                    push_lossy(&mut out, &bytes[start..i]);
                    out.push_str(RESET);
                } else {
                    out.push_str(ERR_C);
                    push_lossy(&mut out, &bytes[start..]);
                    out.push_str(RESET);
                }
            } else {
                // $VAR — read identifier chars (or special: $?, $0, $$, …)
                i += 1;
                while i < n && (bytes[i].is_ascii_alphanumeric() || bytes[i] == b'_') {
                    i += 1;
                }
                out.push_str(VAR_C);
                push_lossy(&mut out, &bytes[start..i]);
                out.push_str(RESET);
            }
            at_cmd_pos = false;
            continue;
        }

        // ── Operators ─────────────────────────────────────────────────
        if matches!(c, b'|' | b'>' | b'<' | b'&' | b';' | b'(' | b')' | b'{' | b'}') {
            out.push(c as char);
            i += 1;

            // Consume second char of compound operators
            let second: Option<u8> = if i < n {
                let nx = bytes[i];
                let is_compound = matches!((c, nx),
                    (b'|', b'|') | (b'&', b'&') |
                    (b';', b';') | (b'>', b'>') | (b'>', b'&'));
                if is_compound { out.push(nx as char); i += 1; Some(nx) }
                else { None }
            } else {
                None
            };

            at_cmd_pos = match (c, second) {
                (b'|', _)          => true,  // | or ||
                (b'&', Some(b'&')) => true,  // &&
                (b';', _)          => true,  // ; or ;;
                (b'(', _)          => true,  // subshell open
                (b'{', _)          => true,  // compound command open
                _                  => false, // >, <, >>, >&, ), }, &
            };
            continue;
        }

        // ── Word token ────────────────────────────────────────────────
        let word_start = i;
        while i < n && !is_word_end(bytes[i]) { i += 1; }

        if i == word_start {
            // unknown byte — emit as-is and advance
            out.push(c as char);
            i += 1;
            continue;
        }

        let word = match std::str::from_utf8(&bytes[word_start..i]) {
            Ok(w) => w,
            Err(_) => {
                push_lossy(&mut out, &bytes[word_start..i]);
                at_cmd_pos = false;
                continue;
            }
        };

        if at_cmd_pos {
            // Detect assignment: VAR=value or VAR+=value — no color, keep cmd pos
            if word.contains('=') {
                let eq = word.find('=').unwrap_or(0);
                let lhs = word[..eq].trim_end_matches('+');
                if !lhs.is_empty() && lhs.chars().all(|c| c.is_alphanumeric() || c == '_') {
                    out.push_str(word);
                    // at_cmd_pos stays true (VAR=val cmd is valid)
                    continue;
                }
            }

            if OPENS.contains(&word) {
                depth += 1;
                out.push_str(KEYWORD_C);
                out.push_str(word);
                out.push_str(RESET);
                at_cmd_pos = true; // condition/loop body follows
            } else if CLOSES.contains(&word) {
                depth -= 1;
                out.push_str(if depth < 0 { NEG_KW_C } else { KEYWORD_C });
                out.push_str(word);
                out.push_str(RESET);
                at_cmd_pos = false;
            } else if CMD_NEXT.contains(&word) {
                out.push_str(KEYWORD_C);
                out.push_str(word);
                out.push_str(RESET);
                at_cmd_pos = true; // body command follows
            } else if word == "function" {
                out.push_str(KEYWORD_C);
                out.push_str(word);
                out.push_str(RESET);
                at_cmd_pos = false; // function name follows, not a command
            } else {
                // Command name — look up in builtins/PATH
                let color = if command_exists(word) { CMD_OK_C } else { CMD_ERR_C };
                out.push_str(color);
                out.push_str(word);
                out.push_str(RESET);
                at_cmd_pos = false;
            }
        } else {
            // Argument position
            if word.len() > 1 && word.starts_with('-') {
                out.push_str(FLAG_C);
                out.push_str(word);
                out.push_str(RESET);
            } else if word.starts_with('/') || word.starts_with("./")
                   || word.starts_with("../") || word == "~" || word.starts_with("~/")
            {
                out.push_str(PATH_C);
                out.push_str(word);
                out.push_str(RESET);
            } else {
                out.push_str(word);
            }
        }
    }

    // Trailing amber hint when a block is left open on this line
    if depth > 0 {
        out.push_str("\x1b[33m");
    }

    out
}
