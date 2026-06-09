// Terminal line editor — ported from archive/src/input.c

use std::os::unix::io::BorrowedFd;
use nix::sys::termios::{self, LocalFlags, InputFlags, SetArg, SpecialCharacterIndices};
use nix::unistd;

use crate::shell::config::config_load;
use crate::shell::highlight::highlight;
use crate::shell::history::{history_get, history_count, history_search_prefix, history_search_multi};

const STDIN:  i32 = 0;
const STDOUT: i32 = 1;
const MAX_BUF: usize = 16384;
const CONT_PROMPT: &str = "> ";
const CONT_PROMPT_ANSI: &str = "\x1b[2;37m> \x1b[0m";

// ─────────────────────────────────────────────────────────────────────────────
//  Raw mode RAII
// ─────────────────────────────────────────────────────────────────────────────

struct RawMode {
    orig: nix::sys::termios::Termios,
}

impl RawMode {
    fn enter() -> Option<Self> {
        // SAFETY: fd 0 is always stdin in a Unix process.
        let fd = unsafe { BorrowedFd::borrow_raw(STDIN) };
        let orig = termios::tcgetattr(fd).ok()?;
        let mut raw = orig.clone();
        raw.local_flags &= !(LocalFlags::ICANON | LocalFlags::ECHO |
                              LocalFlags::ISIG  | LocalFlags::IEXTEN);
        raw.input_flags &= !(InputFlags::IXON   | InputFlags::ICRNL  |
                              InputFlags::BRKINT | InputFlags::INPCK  |
                              InputFlags::ISTRIP);
        raw.control_chars[SpecialCharacterIndices::VMIN  as usize] = 1;
        raw.control_chars[SpecialCharacterIndices::VTIME as usize] = 0;
        // SAFETY: fd 0 is stdin.
        let fd = unsafe { BorrowedFd::borrow_raw(STDIN) };
        termios::tcsetattr(fd, SetArg::TCSAFLUSH, &raw).ok()?;
        // Enable bracketed paste so multi-line pastes don't submit mid-paste.
        // SAFETY: writing to STDOUT_FILENO, which is always open.
        unsafe { libc::write(STDOUT, b"\x1b[?2004h".as_ptr() as *const libc::c_void, 8); }
        Some(RawMode { orig })
    }
}

impl Drop for RawMode {
    fn drop(&mut self) {
        // Disable bracketed paste before restoring terminal settings.
        // SAFETY: writing to STDOUT_FILENO, which is always open.
        unsafe { libc::write(STDOUT, b"\x1b[?2004l".as_ptr() as *const libc::c_void, 8); }
        // SAFETY: fd 0 is stdin.
        let fd = unsafe { BorrowedFd::borrow_raw(STDIN) };
        let _ = termios::tcsetattr(fd, SetArg::TCSAFLUSH, &self.orig);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Low-level I/O
// ─────────────────────────────────────────────────────────────────────────────

fn write_bytes(buf: &[u8]) {
    if buf.is_empty() { return; }
    // SAFETY: buf is a valid slice; STDOUT_FILENO is always open.
    unsafe { libc::write(STDOUT, buf.as_ptr() as *const libc::c_void, buf.len()); }
}

#[inline] fn write_str(s: &str) { write_bytes(s.as_bytes()); }

fn read_byte() -> Option<u8> {
    let mut b = 0u8;
    // SAFETY: b is a valid 1-byte mutable buffer; STDIN_FILENO is always open.
    let n = unsafe { libc::read(STDIN, &mut b as *mut u8 as *mut libc::c_void, 1) };
    if n == 1 { Some(b) } else { None }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Terminal width
// ─────────────────────────────────────────────────────────────────────────────

fn terminal_width() -> usize {
    // SAFETY: TIOCGWINSZ on fd 1 is a standard, safe ioctl.
    unsafe {
        let mut ws: libc::winsize = std::mem::zeroed();
        if libc::ioctl(STDOUT, libc::TIOCGWINSZ, &mut ws) == 0 && ws.ws_col > 0 {
            ws.ws_col as usize
        } else {
            80
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  UTF-8 / ANSI helpers (ported from input.c)
// ─────────────────────────────────────────────────────────────────────────────

fn ansi_display_len(s: &str) -> usize {
    let bytes = s.as_bytes();
    let mut cols: usize = 0;
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == 0x1b {
            i += 1;
            if i < bytes.len() && bytes[i] == b'[' {
                i += 1;
                while i < bytes.len() && !bytes[i].is_ascii_alphabetic() { i += 1; }
                if i < bytes.len() { i += 1; }
            }
            continue;
        }
        let c = bytes[i];
        if c < 0x80 { cols += 1; i += 1; }
        else if c < 0xC0 { i += 1; }          // continuation
        else if c < 0xE0 { cols += 1; i += 2; }
        else if c < 0xF0 { cols += 1; i += 3; }
        else              { cols += 1; i += 4; }
    }
    cols
}

fn utf8_display_len(buf: &[u8]) -> usize {
    let mut cols: usize = 0;
    let mut i = 0;
    while i < buf.len() {
        let c = buf[i];
        if c < 0x80 { cols += 1; i += 1; }
        else if c < 0xC0 { i += 1; }
        else if c < 0xE0 { cols += 1; i += 2; }
        else if c < 0xF0 { cols += 1; i += 3; }
        else              { cols += 1; i += 4; }
    }
    cols
}

/// Number of bytes for the UTF-8 character starting at `pos`.
fn utf8_char_len(buf: &[u8], pos: usize) -> usize {
    if pos >= buf.len() { return 0; }
    let c = buf[pos];
    if c < 0x80 { 1 }
    else if c < 0xC0 { 1 }
    else if c < 0xE0 { 2 }
    else if c < 0xF0 { 3 }
    else { 4 }
}

/// Number of bytes for the UTF-8 character ending just before `pos`.
fn utf8_prev_char_len(buf: &[u8], pos: usize) -> usize {
    if pos == 0 { return 0; }
    let mut back: usize = 1;
    while back < pos && back < 4 && (buf[pos - back] & 0xC0) == 0x80 { back += 1; }
    back
}

/// Byte position of the start of the word ending at `pos`.
fn word_start(buf: &[u8], pos: usize) -> usize {
    let mut i = pos;
    while i > 0 && buf[i - 1] != b' ' { i -= 1; }
    i
}

// ─────────────────────────────────────────────────────────────────────────────
//  Wrapping-aware cursor helpers (Bug 2 fix)
// ─────────────────────────────────────────────────────────────────────────────

/// Returns (screen_row, screen_col) for a cursor that is `cursor_chars` display
/// columns past the start of the prompt, given a terminal width of `tw`.
fn cursor_screen_pos(prompt_cols: usize, cursor_chars: usize, tw: usize) -> (usize, usize) {
    if tw == 0 { return (0, 0); }
    let total = prompt_cols + cursor_chars;
    (total / tw, total % tw)
}

/// How many physical screen rows a single logical line occupies.
fn screen_rows_for_line(prompt_cols: usize, line_display_len: usize, tw: usize) -> usize {
    if tw == 0 { return 1; }
    let total = prompt_cols + line_display_len;
    if total == 0 { return 1; }
    (total + tw - 1) / tw
}

// ─────────────────────────────────────────────────────────────────────────────
//  Block-depth tracking (ported from input.c)
// ─────────────────────────────────────────────────────────────────────────────

fn ml_depth_delta(line: &str) -> i32 {
    let tr = line.trim_start_matches(|c| c == ' ' || c == '\t');
    if tr.is_empty() || tr.starts_with('#') { return 0; }

    static OPEN:  &[&str] = &["if","while","until","for","case"];
    static CLOSE: &[&str] = &["fi","done","esac"];

    let mut delta: i32 = 0;
    let mut p = tr;
    loop {
        let p2 = p.trim_start_matches(|c| c == ' ' || c == '\t' || c == ';');
        if p2.is_empty() { break; }
        let end = p2.find(|c| c == ' ' || c == '\t' || c == ';').unwrap_or(p2.len());
        let word = &p2[..end];
        // function(): word ending in "()" → opens a block
        if word.len() >= 3 && word.ends_with("()") {
            delta += 1;
            // skip optional '{' after the ()
            let rest = p2[end..].trim_start_matches(|c| c == ' ' || c == '\t');
            if rest.starts_with('{') {
                p = &rest[1..];
            } else {
                p = &p2[end..];
            }
            continue;
        }
        if word == "{" { delta += 1; }
        else if word == "}" { delta -= 1; }
        else {
            for kw in OPEN  { if *kw == word { delta += 1; break; } }
            for kw in CLOSE { if *kw == word { delta -= 1; break; } }
        }
        p = &p2[end..];
    }
    delta
}

fn ml_count_depth(buf: &[u8], len: usize) -> i32 {
    let s = std::str::from_utf8(&buf[..len]).unwrap_or("");
    s.split('\n').map(ml_depth_delta).sum()
}

// ─────────────────────────────────────────────────────────────────────────────
//  Rendering (ported from input.c)
// ─────────────────────────────────────────────────────────────────────────────

/// Render a single line segment with syntax highlighting.
/// Uses absolute cursor positioning so long lines that wrap display correctly.
fn render(prompt: &str, buf: &[u8], len: usize, pos: usize, hl: bool) {
    let tw = terminal_width();
    let prompt_cols = ansi_display_len(prompt);

    write_str("\r");
    write_str(prompt);
    let raw = std::str::from_utf8(&buf[..len]).unwrap_or("");
    if hl {
        let colored = highlight(raw);
        if colored.is_empty() {
            write_bytes(&buf[..len]);
        } else {
            write_str(&colored);
        }
    } else {
        write_bytes(&buf[..len]);
    }
    write_str("\x1b[K");

    // Absolute positioning: compute which screen row/col the cursor belongs on
    // after considering line-wrap.  ANSI escapes in the highlighted text do not
    // occupy columns, so we use raw byte display lengths for arithmetic.
    let end_display    = utf8_display_len(&buf[..len]);
    let cursor_display = utf8_display_len(&buf[..pos]);

    let (end_row,    _)          = cursor_screen_pos(prompt_cols, end_display,    tw);
    let (cursor_row, cursor_col) = cursor_screen_pos(prompt_cols, cursor_display, tw);

    let rows_up = end_row.saturating_sub(cursor_row);
    if rows_up > 0 {
        write_str(&format!("\x1b[{}A", rows_up));
    }
    write_str(&format!("\r\x1b[{}C", cursor_col));
}

fn count_newlines(buf: &[u8], len: usize) -> usize {
    buf[..len].iter().filter(|&&b| b == b'\n').count()
}

/// Multiline render.  Returns new `ml_prev_rows` in SCREEN rows (accounting for wrap).
fn ml_render(prompt: &str, buf: &[u8], len: usize, pos: usize,
             ml_prev_rows: usize, hl: bool) -> usize {
    if ml_prev_rows > 1 {
        write_str(&format!("\x1b[{}A", ml_prev_rows - 1));
    }

    let tw = terminal_width();
    let cursor_line = buf[..pos].iter().filter(|&&b| b == b'\n').count();

    let mut seg_start: usize = 0;
    let mut line_idx: usize  = 0;
    let mut total_screen_rows: usize = 0;

    let mut i: usize = 0;
    loop {
        let at_end = i == len;
        if !at_end && buf[i] != b'\n' { i += 1; continue; }

        let seg_len = i - seg_start;
        let pfx = if line_idx == 0 { prompt } else { CONT_PROMPT_ANSI };
        let pfx_display = if line_idx == 0 { ansi_display_len(prompt) } else { CONT_PROMPT.len() };
        let seg_display = utf8_display_len(&buf[seg_start..seg_start + seg_len]);

        write_str("\r");
        write_str(pfx);
        if seg_len > 0 {
            let raw = std::str::from_utf8(&buf[seg_start..seg_start + seg_len]).unwrap_or("");
            if hl {
                let colored = highlight(raw);
                if colored.is_empty() {
                    write_bytes(&buf[seg_start..seg_start + seg_len]);
                } else {
                    write_str(&colored);
                }
            } else {
                write_bytes(&buf[seg_start..seg_start + seg_len]);
            }
        }
        write_str("\x1b[K");

        // Count screen rows this segment occupies (wrapping included)
        let seg_screen_rows = screen_rows_for_line(pfx_display, seg_display, tw);
        total_screen_rows += seg_screen_rows;

        if !at_end {
            write_str("\r\n");
        }

        line_idx += 1;
        seg_start = i + 1;
        if at_end { break; }
        i += 1;
    }

    write_str("\x1b[J");

    // Move from the last rendered segment up to the cursor line.
    // We need to go up by the screen rows occupied by segments BELOW the cursor line.
    let mut screen_rows_below: usize = 0;
    {
        let mut si = 0;
        let mut li = 0;
        let mut ss = 0usize;
        loop {
            let at_end = si == len;
            if !at_end && buf[si] != b'\n' { si += 1; continue; }
            let sl = si - ss;
            let pd = if li == 0 { ansi_display_len(prompt) } else { CONT_PROMPT.len() };
            let sd = utf8_display_len(&buf[ss..ss + sl]);
            if li > cursor_line {
                screen_rows_below += screen_rows_for_line(pd, sd, tw);
            }
            li += 1;
            ss = si + 1;
            if at_end { break; }
            si += 1;
        }
    }

    if screen_rows_below > 0 {
        write_str(&format!("\x1b[{}A", screen_rows_below));
    }

    // Position cursor at the correct column on the cursor line (wrap-aware).
    let cursor_line_start = if cursor_line == 0 {
        0
    } else {
        let mut nl = 0usize;
        let mut cls = 0usize;
        for (bi, &byte) in buf[..len].iter().enumerate() {
            if nl == cursor_line { cls = bi; break; }
            if byte == b'\n' { nl += 1; }
        }
        cls
    };
    let cursor_byte_col = pos - cursor_line_start;
    let pfx_cols = if cursor_line == 0 { ansi_display_len(prompt) } else { CONT_PROMPT.len() };
    let cursor_display_col = utf8_display_len(&buf[cursor_line_start..cursor_line_start + cursor_byte_col]);

    let (extra_rows, col_on_row) = cursor_screen_pos(pfx_cols, cursor_display_col, tw);
    if extra_rows > 0 {
        write_str(&format!("\x1b[{}B", extra_rows));
    }
    write_str(&format!("\r\x1b[{}C", col_on_row));

    total_screen_rows
}

/// Dispatcher: single-line uses render() + ghost, multiline uses ml_render().
/// Returns new `ml_prev_rows` (in SCREEN rows, accounting for wrap).
fn render_ml(prompt: &str, buf: &[u8], len: usize, pos: usize,
             ml_prev_rows: usize, hl: bool, suggestions: bool) -> usize {
    let has_newline = buf[..len].contains(&b'\n');
    if !has_newline {
        render(prompt, buf, len, pos, hl);
        // Ghost suggestion only when cursor is at end and suggestions enabled
        if suggestions && pos == len && len > 0 {
            let raw = std::str::from_utf8(&buf[..len]).unwrap_or("");
            if let Some(sug) = find_suggestion(raw) {
                let sug_len_bytes = sug.len();
                if sug_len_bytes > len {
                    let ghost = &sug[len..];
                    let ghost_cols = utf8_display_len(ghost.as_bytes());
                    if ghost_cols > 0 {
                        write_str("\x1b[2;37m");
                        write_str(ghost);
                        write_str("\x1b[0m");
                        // Move cursor back over the ghost text using absolute positioning
                        // so it works even if the ghost caused a line wrap.
                        let tw = terminal_width();
                        let prompt_cols = ansi_display_len(prompt);
                        let cursor_display = utf8_display_len(&buf[..pos]);
                        let ghost_end = cursor_display + ghost_cols;
                        let (end_row,    _)          = cursor_screen_pos(prompt_cols, ghost_end,      tw);
                        let (cursor_row, cursor_col) = cursor_screen_pos(prompt_cols, cursor_display, tw);
                        let rows_up = end_row.saturating_sub(cursor_row);
                        if rows_up > 0 { write_str(&format!("\x1b[{}A", rows_up)); }
                        write_str(&format!("\r\x1b[{}C", cursor_col));
                    }
                }
            }
        }
        // Return the actual number of screen rows this line occupies.
        let tw = terminal_width();
        let prompt_cols = ansi_display_len(prompt);
        let display_len = utf8_display_len(&buf[..len]);
        screen_rows_for_line(prompt_cols, display_len, tw)
    } else {
        ml_render(prompt, buf, len, pos, ml_prev_rows, hl)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Ghost suggestion (ported from input.c)
// ─────────────────────────────────────────────────────────────────────────────

fn find_suggestion(buf_str: &str) -> Option<String> {
    if buf_str.is_empty() { return None; }
    if let Some(h) = history_search_prefix(buf_str) {
        if !h.contains('\n') {
            return Some(h);
        }
    }
    // Fall back to glob on last word
    let ws = buf_str.rfind(|c| c == ' ').map(|i| i + 1).unwrap_or(0);
    let word = &buf_str[ws..];
    if word.len() < 2 { return None; }
    let pattern = format!("{}*", word);
    if let Ok(mut paths) = glob::glob(&pattern) {
        if let Some(Ok(path)) = paths.next() {
            return Some(path.to_string_lossy().into_owned());
        }
    }
    None
}

// ─────────────────────────────────────────────────────────────────────────────
//  clear_input_area (ported from input.c)
// ─────────────────────────────────────────────────────────────────────────────

fn clear_input_area(ml_prev_rows: &mut usize, panel_rows: &mut usize) {
    if *panel_rows > 0 {
        write_str(&format!("\x1b[{}B", panel_rows));
    }
    let total_up = (*ml_prev_rows - 1) + *panel_rows;
    if total_up > 0 {
        write_str(&format!("\x1b[{}A", total_up));
    }
    write_str("\r\x1b[J");
    *ml_prev_rows = 1;
    *panel_rows   = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  panel_show_history (ported from input.c)
// ─────────────────────────────────────────────────────────────────────────────

fn panel_show_history(hist_off: usize, panel_rows: &mut usize) {
    if *panel_rows > 0 {
        for _ in 0..*panel_rows { write_str("\x1b[B"); }
        write_str("\x1b[J");
        write_str(&format!("\x1b[{}A", panel_rows));
        *panel_rows = 0;
    }
    let total = history_count();
    let mut entries: Vec<String> = Vec::new();
    for i in 1..=10 {
        match history_get(i) { Some(e) => entries.push(e), None => break }
    }
    if entries.is_empty() { return; }
    let mut rows: usize = 0;
    for (i, entry) in entries.iter().enumerate() {
        let is_sel = (i + 1) == hist_off;
        let abs_index = total.saturating_sub(i);
        write_str("\n\x1b[K");
        rows += 1;
        if is_sel {
            write_str("\x1b[1;32m\u{25b6} \x1b[0m");
            write_str(&format!("\x1b[1;32m{:2}\x1b[0m  ", abs_index));
            write_str("\x1b[1m");
        } else {
            write_str("  ");
            write_str(&format!("\x1b[2;37m{:2}\x1b[0m  ", abs_index));
        }
        for ch in entry.chars() {
            if ch == '\n' {
                write_str("\n\x1b[K    ");
                rows += 1;
            } else {
                let mut tmp = [0u8; 4];
                write_bytes(ch.encode_utf8(&mut tmp).as_bytes());
            }
        }
        if is_sel { write_str("\x1b[0m"); }
    }
    write_str(&format!("\x1b[{}A", rows));
    *panel_rows = rows;
}

// ─────────────────────────────────────────────────────────────────────────────
//  panel_rebuild (ported from input.c, simplified — no aliases/frecency)
// ─────────────────────────────────────────────────────────────────────────────

fn panel_rebuild(buf: &[u8], len: usize, pos: usize) -> Vec<String> {
    if len == 0 { return Vec::new(); }
    let buf_str = std::str::from_utf8(&buf[..len]).unwrap_or("");

    let first_space = buf_str.find(' ');
    let mut items: Vec<String> = Vec::with_capacity(64);

    if first_space.is_none() {
        // completing command name
        let word = buf_str;
        let wlen = word.len();
        let builtins = ["cd","exit","export","pwd","echo","alias","unalias",
                        "source","jobs","fg","bg","set","unset","readonly","local",
                        "declare","read","printf","trap","eval","type","hash","wait"];
        for b in builtins {
            if b.starts_with(word) && !items.iter().any(|x| x == b) {
                items.push(b.to_string());
            }
        }
        // PATH executables
        if let Ok(path_env) = std::env::var("PATH") {
            for dir in path_env.split(':') {
                if let Ok(rd) = std::fs::read_dir(dir) {
                    for ent in rd.flatten() {
                        let name = ent.file_name();
                        let name_str = name.to_string_lossy();
                        if name_str.starts_with(word) && !items.iter().any(|x| x == name_str.as_ref()) {
                            if let Ok(md) = ent.metadata() {
                                use std::os::unix::fs::PermissionsExt;
                                if md.permissions().mode() & 0o111 != 0 {
                                    items.push(name_str.to_string());
                                    if items.len() >= 60 { break; }
                                }
                            }
                        }
                    }
                }
                if items.len() >= 60 { break; }
            }
        }
        // dynamic completions fallback
        if items.is_empty() {
            items = crate::shell::completions::complete(buf_str, pos);
        }
    } else {
        // completing argument
        let fs = first_space.unwrap();
        let cmd = &buf_str[..fs];
        // current word under cursor
        let ws = buf[..pos].iter().rposition(|&b| b == b' ').map(|i| i + 1).unwrap_or(0);
        let word = std::str::from_utf8(&buf[ws..pos]).unwrap_or("");

        if cmd == "cd" {
            // separator + glob subdirs
            items.push("\u{2605} frecency".to_string());
            let pattern = if word.is_empty() { "./*".to_string() } else { format!("{}*", word) };
            if let Ok(paths) = glob::glob(&pattern) {
                for p in paths.flatten() {
                    if p.is_dir() {
                        items.push(p.to_string_lossy().into_owned());
                        if items.len() >= 60 { break; }
                    }
                }
            }
            return items;
        }

        // dynamic completions
        let dyn_items = crate::shell::completions::complete(buf_str, pos);
        if !dyn_items.is_empty() {
            return dyn_items;
        }

        // glob files/dirs matching word
        let pattern = if word.is_empty() { "./*".to_string() } else { format!("{}*", word) };
        if let Ok(paths) = glob::glob(&pattern) {
            for p in paths.flatten() {
                items.push(p.to_string_lossy().into_owned());
                if items.len() >= 60 { break; }
            }
        }
    }
    items
}

// ─────────────────────────────────────────────────────────────────────────────
//  panel_render (ported from input.c)
// ─────────────────────────────────────────────────────────────────────────────

fn is_separator(s: &str) -> bool {
    // ★ (U+2605, UTF-8: E2 98 85) or 📁 (U+1F4C1, UTF-8: F0 9F 93 81)
    let b = s.as_bytes();
    (b.len() >= 3 && b[0] == 0xE2 && b[1] == 0x98 && b[2] == 0x85) ||
    (b.len() >= 4 && b[0] == 0xF0 && b[1] == 0x9F)
}

fn panel_render(items: &[String], sel: i32, panel_rows: &mut usize) {
    // clear previous panel rows
    if *panel_rows > 0 {
        write_str("\r");
        for _ in 0..*panel_rows { write_str("\x1b[B\x1b[K"); }
        write_str(&format!("\x1b[{}A", panel_rows));
        *panel_rows = 0;
    }
    if items.is_empty() { return; }
    let tw = terminal_width();
    let col_width = items.iter().map(|s| utf8_display_len(s.as_bytes())).max().unwrap_or(1) + 2;
    let cols = (tw / col_width).max(1);
    let max_rows = 4;
    let visible = items.len().min(cols * max_rows);
    let mut rows: usize = 0;
    for (i, item) in items[..visible].iter().enumerate() {
        if i % cols == 0 { write_str("\n\x1b[K"); rows += 1; }
        if is_separator(item) {
            write_str("\x1b[2;33m");
            write_str(item);
            write_str("\x1b[0m");
        } else {
            let highlighted = i as i32 == sel;
            write_str(if highlighted { "\x1b[7m" } else { "\x1b[2;36m" });
            write_str(item);
            write_str("\x1b[0m");
        }
        let pad = col_width.saturating_sub(utf8_display_len(item.as_bytes()));
        for _ in 0..pad { write_str(" "); }
    }
    write_str(&format!("\x1b[{}A", rows));
    *panel_rows = rows;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Ctrl+R interactive history search (ported from input.c)
// ─────────────────────────────────────────────────────────────────────────────

fn write_highlighted(s: &str, query: &str) {
    if query.is_empty() { write_str(s); return; }
    let qb = query.as_bytes();
    let sb = s.as_bytes();
    let mut i = 0;
    while i < sb.len() {
        if i + qb.len() <= sb.len() {
            let slice = &sb[i..i + qb.len()];
            let matches = slice.iter().zip(qb.iter())
                .all(|(a, b)| a.to_ascii_lowercase() == b.to_ascii_lowercase());
            if matches {
                write_str("\x1b[1;33m");
                write_bytes(slice);
                write_str("\x1b[0m");
                i += qb.len();
                continue;
            }
        }
        write_bytes(&sb[i..i+1]);
        i += 1;
    }
}

fn ctr_render_prompt(query: &str, results: &[String], sel: usize) {
    write_str("\r\x1b[K");
    write_str("\x1b[2;37m(search) \x1b[0m");
    if !results.is_empty() && sel < results.len() {
        let res = &results[sel];
        if let Some(nl) = res.find('\n') {
            write_bytes(res[..nl].as_bytes());
            write_str("\x1b[2;37m\u{2026}\x1b[0m");
        } else {
            write_highlighted(res, query);
        }
    }
    write_str("  \x1b[2;37m>\x1b[0m  ");
    write_str("\x1b[1;36m");
    write_str(query);
    write_str("\x1b[0m\x1b[K");
}

fn ctr_clear_list(list_rows: &mut usize) {
    if *list_rows == 0 { return; }
    for _ in 0..*list_rows { write_str("\x1b[B"); }
    write_str("\x1b[J");
    write_str(&format!("\x1b[{}A", list_rows));
    *list_rows = 0;
}

fn ctr_render_list(results: &[String], ids: &[usize], sel: usize, list_rows: &mut usize, query: &str) {
    ctr_clear_list(list_rows);
    if results.is_empty() { return; }
    let show = results.len().min(8);
    for i in 0..show {
        write_str("\n\x1b[K");
        *list_rows += 1;
        if i == sel {
            write_str(&format!("\x1b[1;32m\u{25b6} {:2}\x1b[0m  \x1b[1m", ids[i]));
        } else {
            write_str(&format!("  \x1b[2;37m{:2}\x1b[0m  ", ids[i]));
        }
        for ch in results[i].chars() {
            if ch == '\n' {
                write_str(" ");
            } else {
                let mut tmp = [0u8; 4];
                let s = ch.encode_utf8(&mut tmp);
                // highlight query match
                let sb = s.as_bytes();
                let qb = query.as_bytes();
                if !qb.is_empty() && sb.len() == 1 && i + qb.len() <= results[i].len() {
                    write_highlighted(s, query);
                } else {
                    write_bytes(sb);
                }
            }
        }
        if i == sel { write_str("\x1b[0m"); }
    }
    write_str(&format!("\x1b[{}A", show));
}

fn search_history_interactive() -> Option<String> {
    let mut query = String::new();
    let mut sel: usize = 0;
    let mut list_rows: usize = 0;
    let (mut results, mut ids) = (Vec::<String>::new(), Vec::<usize>::new());

    ctr_render_prompt(&query, &results, sel);
    ctr_render_list(&results, &ids, sel, &mut list_rows, &query);
    ctr_render_prompt(&query, &results, sel);

    loop {
        let c = read_byte()?;
        match c {
            b'\r' | b'\n' => {
                let ret = if !results.is_empty() && sel < results.len() {
                    Some(results[sel].clone())
                } else {
                    None
                };
                ctr_clear_list(&mut list_rows);
                write_str("\r\x1b[K\r\n");
                return ret;
            }
            3 => { // Ctrl+C
                ctr_clear_list(&mut list_rows);
                write_str("\r\x1b[K\r\n");
                return None;
            }
            27 => { // ESC
                let b1 = read_byte()?;
                if b1 == b'[' {
                    let b2 = read_byte()?;
                    match b2 {
                        b'A' => { // up
                            if !results.is_empty() {
                                sel = sel.saturating_sub(1);
                                if sel == 0 && !results.is_empty() { sel = 0; }
                                if results.len() > 0 { sel = (sel + results.len() - 1) % results.len(); }
                            }
                        }
                        b'B' => { // down
                            if !results.is_empty() { sel = (sel + 1) % results.len(); }
                        }
                        _ => {}
                    }
                } else {
                    // plain ESC — cancel
                    ctr_clear_list(&mut list_rows);
                    write_str("\r\x1b[K\r\n");
                    return None;
                }
                ctr_render_prompt(&query, &results, sel);
                ctr_render_list(&results, &ids, sel, &mut list_rows, &query);
                ctr_render_prompt(&query, &results, sel);
            }
            18 => { // Ctrl+R — cycle forward
                if !results.is_empty() { sel = (sel + 1) % results.len(); }
                ctr_render_prompt(&query, &results, sel);
                ctr_render_list(&results, &ids, sel, &mut list_rows, &query);
                ctr_render_prompt(&query, &results, sel);
            }
            127 | 8 => { // Backspace
                if !query.is_empty() {
                    query.pop();
                    sel = 0;
                    if !query.is_empty() {
                        (results, ids) = history_search_multi(&query, 8);
                    } else {
                        results.clear(); ids.clear();
                    }
                }
                ctr_render_prompt(&query, &results, sel);
                ctr_render_list(&results, &ids, sel, &mut list_rows, &query);
                ctr_render_prompt(&query, &results, sel);
            }
            32..=126 => { // printable ASCII
                query.push(c as char);
                sel = 0;
                (results, ids) = history_search_multi(&query, 8);
                ctr_render_prompt(&query, &results, sel);
                ctr_render_list(&results, &ids, sel, &mut list_rows, &query);
                ctr_render_prompt(&query, &results, sel);
            }
            _ => {}
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

pub fn read_line(prompt: &str) -> Option<String> {
    // Script / pipe mode: fall back to simple stdin read
    // SAFETY: fd 0 is stdin.
    let tty = unistd::isatty(STDIN).unwrap_or(false);
    if !tty {
        use std::io::{self, BufRead};
        print!("{}", prompt);
        let _ = {
            use std::io::Write;
            std::io::stdout().flush()
        };
        let stdin = io::stdin();
        let mut line = String::new();
        return match stdin.lock().read_line(&mut line) {
            Ok(0) => None,
            Ok(_) => {
                if line.ends_with('\n') { line.pop(); }
                if line.ends_with('\r') { line.pop(); }
                Some(line)
            }
            Err(_) => None,
        };
    }

    let config = config_load();
    let hl          = config.features.highlighting;
    let suggestions = config.features.suggestions;
    let panel_en    = config.features.completions;

    // Enter raw mode — restored on drop.
    let _raw = RawMode::enter()?;

    let mut buf: Vec<u8> = vec![0u8; MAX_BUF];
    let mut len:     usize = 0;
    let mut pos:     usize = 0;
    let mut hist_off: usize = 0;
    let mut saved:   Vec<u8> = Vec::new();

    let mut panel_items: Vec<String> = Vec::new();
    let mut panel_sel:   i32 = -1;
    let mut panel_rows:  usize = 0;
    let mut ml_prev_rows: usize = 1;
    let mut in_paste: bool = false;

    // Initial render
    write_str(prompt);

    loop {
        let c = match read_byte() {
            Some(b) => b,
            None => {
                write_str("\r\n");
                return None;
            }
        };

        // ── TAB ─────────────────────────────────────────────────────────────
        if c == b'\t' {
            // if ghost suggestion active and cursor at end, accept it
            if suggestions && pos == len && len > 0 {
                let raw = std::str::from_utf8(&buf[..len]).unwrap_or("");
                if let Some(sug) = find_suggestion(raw) {
                    if sug.len() > len {
                        let sug_bytes = sug.as_bytes();
                        let sug_len = sug_bytes.len();
                        if sug_len < MAX_BUF - 1 {
                            buf[..sug_len].copy_from_slice(&sug_bytes[..sug_len]);
                            len = sug_len; pos = len; buf[len] = 0;
                            if panel_en {
                                panel_items = panel_rebuild(&buf, len, pos);
                            }
                            panel_sel = -1;
                            if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                            continue;
                        }
                    }
                }
            }

            if panel_en && panel_items.is_empty() {
                // bell
                // SAFETY: writing a single byte to STDOUT.
                unsafe { libc::write(STDOUT, b"\x07".as_ptr() as *const libc::c_void, 1); }
                continue;
            }
            if panel_en && panel_items.len() == 1 && panel_sel == -1 {
                // insert single match
                let ws = buf[..pos].iter().rposition(|&b| b == b' ').map(|i| i + 1).unwrap_or(0);
                let match_bytes = panel_items[0].as_bytes();
                let match_len = match_bytes.len();
                let tail = len - pos;
                if ws + match_len + tail < MAX_BUF - 1 {
                    buf.copy_within(pos..len, ws + match_len);
                    buf[ws..ws + match_len].copy_from_slice(match_bytes);
                    len = ws + match_len + tail;
                    pos = ws + match_len;
                    buf[len] = 0;
                }
                if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
                panel_sel = -1;
                if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                continue;
            }
            if panel_en && !panel_items.is_empty() {
                // count non-separator visible items
                let tw = terminal_width();
                let col_width = panel_items.iter().map(|s| utf8_display_len(s.as_bytes())).max().unwrap_or(1) + 2;
                let cols = (tw / col_width).max(1);
                let visible = panel_items.len().min(cols * 4);
                loop {
                    panel_sel = if panel_sel == -1 { 0 } else { (panel_sel + 1) as i32 % visible as i32 };
                    if (panel_sel as usize) < panel_items.len() {
                        if !is_separator(&panel_items[panel_sel as usize]) { break; }
                    } else { break; }
                }
                panel_render(&panel_items, panel_sel, &mut panel_rows);
                ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                continue;
            }
            continue;
        }

        // ── Ctrl+D ───────────────────────────────────────────────────────────
        if c == 4 {
            if len == 0 {
                panel_render(&[], -1, &mut panel_rows);
                write_str("\r\n");
                return None;
            }
            // delete char at cursor
            if pos < len {
                let char_len = utf8_char_len(&buf, pos);
                buf.copy_within(pos + char_len..len, pos);
                len -= char_len;
                buf[len] = 0;
                if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
                panel_sel = -1;
                if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            }
            continue;
        }

        // ── Ctrl+C ───────────────────────────────────────────────────────────
        if c == 3 {
            if len > 0 { write_str("^C\r\n"); }
            buf[..len].fill(0);
            len = 0; pos = 0; hist_off = 0;
            saved.clear();
            panel_items.clear(); panel_sel = -1;
            panel_render(&[], -1, &mut panel_rows);
            ml_prev_rows = 1;
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── Ctrl+L ───────────────────────────────────────────────────────────
        if c == 12 {
            write_str("\x1b[2J\x1b[H");
            ml_prev_rows = 1;
            panel_rows = 0;
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── Enter ────────────────────────────────────────────────────────────
        if c == b'\r' || c == b'\n' {
            // While pasting, newlines are literal — do not submit the buffer.
            if in_paste {
                if len + 1 < MAX_BUF - 1 {
                    buf.copy_within(pos..len, pos + 1);
                    buf[pos] = b'\n'; len += 1; pos += 1; buf[len] = 0;
                }
                if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
                panel_sel = -1;
                if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                continue;
            }

            // if panel item selected, insert it
            if panel_sel >= 0 && !panel_items.is_empty() {
                let ws = buf[..pos].iter().rposition(|&b| b == b' ').map(|i| i + 1).unwrap_or(0);
                let match_bytes = panel_items[panel_sel as usize].as_bytes();
                let match_len   = match_bytes.len();
                let tail = len - pos;
                if ws + match_len + tail < MAX_BUF - 1 {
                    buf.copy_within(pos..len, ws + match_len);
                    buf[ws..ws + match_len].copy_from_slice(match_bytes);
                    len = ws + match_len + tail;
                    pos = ws + match_len;
                    buf[len] = 0;
                }
                if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
                panel_sel = -1;
                if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                continue;
            }
            // multiline depth check
            let depth = ml_count_depth(&buf, len);
            if depth > 0 {
                if len + 1 < MAX_BUF - 1 {
                    buf.copy_within(pos..len, pos + 1);
                    buf[pos] = b'\n'; len += 1; pos += 1; buf[len] = 0;
                }
                panel_items.clear(); panel_sel = -1;
                if panel_en { panel_render(&[], -1, &mut panel_rows); }
                ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                continue;
            }
            // commit
            if panel_rows > 0 {
                write_str(&format!("\x1b[{}B", panel_rows));
            }
            write_str("\x1b[J");
            if panel_rows > 0 {
                write_str(&format!("\x1b[{}A", panel_rows));
            }
            panel_rows = 0;
            ml_render(prompt, &buf, len, len, ml_prev_rows, hl);
            write_str("\x1b[K\r\n");
            panel_items.clear(); panel_sel = -1;
            let result = std::str::from_utf8(&buf[..len]).unwrap_or("").to_string();
            return Some(result);
        }

        // ── Backspace ────────────────────────────────────────────────────────
        if c == 127 || c == 8 {
            if pos > 0 {
                let back = if buf[pos - 1] == b'\n' { 1 } else { utf8_prev_char_len(&buf, pos) };
                buf.copy_within(pos..len, pos - back);
                len -= back; pos -= back; buf[len] = 0;
                if len == 0 { hist_off = 0; saved.clear(); }
            }
            if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
            panel_sel = -1;
            if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── Ctrl+A ───────────────────────────────────────────────────────────
        if c == 1 {
            while pos > 0 && buf[pos - 1] != b'\n' { pos -= 1; }
            if panel_sel >= 0 { panel_sel = -1; if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); } }
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── Ctrl+E ───────────────────────────────────────────────────────────
        if c == 5 {
            while pos < len && buf[pos] != b'\n' { pos += 1; }
            if panel_sel >= 0 { panel_sel = -1; if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); } }
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── Ctrl+F ───────────────────────────────────────────────────────────
        if c == 6 {
            if pos < len {
                let cl = utf8_char_len(&buf, pos);
                pos = (pos + cl).min(len);
            }
            if panel_sel >= 0 { panel_sel = -1; if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); } }
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── Ctrl+B ───────────────────────────────────────────────────────────
        if c == 2 {
            if pos > 0 {
                let back = utf8_prev_char_len(&buf, pos);
                pos = pos.saturating_sub(back);
            }
            if panel_sel >= 0 { panel_sel = -1; if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); } }
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── Ctrl+K ───────────────────────────────────────────────────────────
        if c == 11 {
            let nl_pos = buf[pos..len].iter().position(|&b| b == b'\n').map(|i| i + pos).unwrap_or(len);
            len = nl_pos; buf[len] = 0;
            if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
            panel_sel = -1;
            if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── Ctrl+U ───────────────────────────────────────────────────────────
        if c == 21 {
            let line_start = {
                let mut i = pos;
                while i > 0 && buf[i - 1] != b'\n' { i -= 1; }
                i
            };
            buf.copy_within(pos..len, line_start);
            len -= pos - line_start;
            pos = line_start;
            buf[len] = 0;
            if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
            panel_sel = -1;
            if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── Ctrl+W ───────────────────────────────────────────────────────────
        if c == 23 {
            if pos > 0 {
                let end = pos;
                while pos > 0 && buf[pos - 1] == b' ' { pos -= 1; }
                while pos > 0 && buf[pos - 1] != b' ' { pos -= 1; }
                buf.copy_within(end..len, pos);
                len -= end - pos;
                buf[len] = 0;
            }
            if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
            panel_sel = -1;
            if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── Ctrl+R ───────────────────────────────────────────────────────────
        if c == 18 {
            let result = search_history_interactive();
            write_str("\r\x1b[J");
            if let Some(r) = result {
                let rb = r.as_bytes();
                let rlen = rb.len().min(MAX_BUF - 1);
                buf[..rlen].copy_from_slice(&rb[..rlen]);
                len = rlen; pos = len; buf[len] = 0;
            } else {
                buf[..len].fill(0);
                len = 0; pos = 0;
            }
            hist_off = 0; ml_prev_rows = 1;
            if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
            panel_sel = -1;
            if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── ESC sequences ────────────────────────────────────────────────────
        if c == 27 {
            let b1 = match read_byte() { Some(b) => b, None => continue };
            let b2 = match read_byte() { Some(b) => b, None => continue };

            if b1 == b'[' {
                // Bracketed paste markers: ESC [ 2 0 0 ~  and  ESC [ 2 0 1 ~
                // We already consumed b1='[' and b2='2'.  Read three more bytes.
                if b2 == b'2' {
                    let b3 = match read_byte() { Some(b) => b, None => continue };
                    let b4 = match read_byte() { Some(b) => b, None => continue };
                    let b5 = match read_byte() { Some(b) => b, None => continue };
                    if b3 == b'0' && b4 == b'0' && b5 == b'~' {
                        in_paste = true;
                    } else if b3 == b'0' && b4 == b'1' && b5 == b'~' {
                        in_paste = false;
                    }
                    // unknown ESC[2…~ sequence — ignore
                    continue;
                }

                // Up arrow
                if b2 == b'A' {
                    if panel_sel >= 0 {
                        panel_sel = (panel_sel - 1).max(-1);
                        if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                        ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                        continue;
                    }
                    // multiline cursor movement
                    let on_first = !buf[..pos].contains(&b'\n');
                    if !on_first && hist_off == 0 {
                        // move cursor to previous logical line
                        let col = buf[..pos].iter().rev().take_while(|&&b| b != b'\n').count();
                        let prev_nl = pos - col - 1; // position of the \n before current line
                        let prev_start = buf[..prev_nl].iter().rposition(|&b| b == b'\n').map(|i| i + 1).unwrap_or(0);
                        let prev_len = prev_nl - prev_start;
                        pos = prev_start + col.min(prev_len);
                        ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                        continue;
                    }
                    // history up
                    if hist_off == 0 { saved = buf[..len].to_vec(); }
                    hist_off += 1;
                    if let Some(h) = history_get(hist_off) {
                        let hb = h.as_bytes();
                        let hlen = hb.len().min(MAX_BUF - 1);
                        buf[..hlen].copy_from_slice(&hb[..hlen]);
                        len = hlen; pos = len; buf[len] = 0;
                    } else {
                        hist_off -= 1;
                    }
                    clear_input_area(&mut ml_prev_rows, &mut panel_rows);
                    panel_show_history(hist_off, &mut panel_rows);
                    ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                    continue;
                }

                // Down arrow
                if b2 == b'B' {
                    if panel_sel == -1 && !panel_items.is_empty() && hist_off == 0 {
                        panel_sel = 0;
                        if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                        ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                        continue;
                    } else if panel_sel >= 0 {
                        panel_sel = (panel_sel + 1) % panel_items.len() as i32;
                        if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                        ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                        continue;
                    }
                    let on_last = !buf[pos..len].contains(&b'\n');
                    if !on_last && hist_off == 0 {
                        let col = buf[..pos].iter().rev().take_while(|&&b| b != b'\n').count();
                        let next_nl = buf[pos..len].iter().position(|&b| b == b'\n').map(|i| i + pos).unwrap_or(len);
                        let next_start = next_nl + 1;
                        let next_end = buf[next_start..len].iter().position(|&b| b == b'\n').map(|i| i + next_start).unwrap_or(len);
                        let next_len = next_end - next_start;
                        pos = (next_start + col.min(next_len)).min(len);
                        ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                        continue;
                    }
                    if hist_off == 0 { continue; }
                    hist_off -= 1;
                    if hist_off == 0 {
                        let slen = saved.len().min(MAX_BUF - 1);
                        buf[..slen].copy_from_slice(&saved[..slen]);
                        len = slen; pos = len; buf[len] = 0;
                    } else if let Some(h) = history_get(hist_off) {
                        let hb = h.as_bytes();
                        let hlen = hb.len().min(MAX_BUF - 1);
                        buf[..hlen].copy_from_slice(&hb[..hlen]);
                        len = hlen; pos = len; buf[len] = 0;
                    }
                    clear_input_area(&mut ml_prev_rows, &mut panel_rows);
                    if hist_off > 0 {
                        panel_show_history(hist_off, &mut panel_rows);
                    } else {
                        if panel_en {
                            panel_items = panel_rebuild(&buf, len, pos);
                            panel_render(&panel_items, panel_sel, &mut panel_rows);
                        }
                    }
                    ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                    continue;
                }

                // Right arrow
                if b2 == b'C' {
                    if panel_sel >= 0 {
                        panel_sel = (panel_sel + 1) % panel_items.len() as i32;
                        if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                        ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                        continue;
                    }
                    if pos < len {
                        let cl = utf8_char_len(&buf, pos);
                        pos = (pos + cl).min(len);
                    }
                    ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                    continue;
                }

                // Left arrow
                if b2 == b'D' {
                    if panel_sel >= 0 {
                        panel_sel = ((panel_sel - 1 + panel_items.len() as i32) % panel_items.len() as i32);
                        if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                        ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                        continue;
                    }
                    if pos > 0 {
                        let back = utf8_prev_char_len(&buf, pos);
                        pos = pos.saturating_sub(back);
                    }
                    ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                    continue;
                }

                // Delete key: ESC [ 3 ~
                if b2 == b'3' {
                    let _ = read_byte(); // consume ~
                    if pos < len {
                        let cl = utf8_char_len(&buf, pos);
                        buf.copy_within(pos + cl..len, pos);
                        len -= cl; buf[len] = 0;
                        if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
                        panel_sel = -1;
                        if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                        ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                    }
                    continue;
                }

            } else if b1 == b'f' || b2 == b'f' {
                // Alt+F — move forward one word
                while pos < len && buf[pos] == b' ' { pos += 1; }
                while pos < len && buf[pos] != b' ' {
                    pos += utf8_char_len(&buf, pos);
                }
                ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                continue;
            } else if b1 == b'b' || b2 == b'b' {
                // Alt+B — move backward one word
                while pos > 0 && buf[pos - 1] == b' ' { pos -= 1; }
                while pos > 0 && buf[pos - 1] != b' ' {
                    pos = pos.saturating_sub(utf8_prev_char_len(&buf, pos));
                }
                ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                continue;
            } else if b1 == b'd' || b2 == b'd' {
                // Alt+D — kill next word
                let start = pos;
                while pos < len && buf[pos] == b' ' { pos += 1; }
                while pos < len && buf[pos] != b' ' {
                    pos += utf8_char_len(&buf, pos);
                }
                buf.copy_within(pos..len, start);
                len -= pos - start;
                pos = start; buf[len] = 0;
                if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
                panel_sel = -1;
                if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                continue;
            } else {
                // Other ESC: dismiss panel
                if panel_sel >= 0 {
                    panel_sel = -1;
                    if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                    ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                }
            }
            continue;
        }

        // ── ASCII printable ──────────────────────────────────────────────────
        if c >= 32 && c < 127 {
            panel_sel = -1;
            if len + 1 < MAX_BUF - 1 {
                buf.copy_within(pos..len, pos + 1);
                buf[pos] = c; len += 1; pos += 1; buf[len] = 0;
            }
            if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
            panel_sel = -1;
            if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
            ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
            continue;
        }

        // ── UTF-8 multi-byte ─────────────────────────────────────────────────
        if c >= 0xC0 && c <= 0xF7 {
            let nb: usize = if c >= 0xF0 { 4 } else if c >= 0xE0 { 3 } else { 2 };
            if len + nb < MAX_BUF - 1 {
                let mut seq = [0u8; 4];
                seq[0] = c;
                let mut ok = true;
                for b in seq[1..nb].iter_mut() {
                    match read_byte() {
                        Some(cb) if (cb & 0xC0) == 0x80 => *b = cb,
                        _ => { ok = false; break; }
                    }
                }
                if ok {
                    buf.copy_within(pos..len, pos + nb);
                    buf[pos..pos + nb].copy_from_slice(&seq[..nb]);
                    len += nb; pos += nb; buf[len] = 0;
                    if panel_en { panel_items = panel_rebuild(&buf, len, pos); }
                    panel_sel = -1;
                    if panel_en { panel_render(&panel_items, panel_sel, &mut panel_rows); }
                    ml_prev_rows = render_ml(prompt, &buf, len, pos, ml_prev_rows, hl, suggestions);
                }
            }
            continue;
        }
    }
}

pub fn is_incomplete(line: &str) -> bool {
    let mut in_single = false;
    let mut in_double = false;
    let mut paren_depth: i32 = 0;
    let mut brace_depth: i32 = 0;
    let chars: Vec<char> = line.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        match chars[i] {
            '\'' if !in_double => { in_single = !in_single; }
            '"'  if !in_single => { in_double = !in_double; }
            '('  if !in_single && !in_double => { paren_depth += 1; }
            ')'  if !in_single && !in_double => { paren_depth -= 1; }
            '{'  if !in_single && !in_double => { brace_depth += 1; }
            '}'  if !in_single && !in_double => { brace_depth -= 1; }
            '\\' if !in_single => { i += 1; }
            _ => {}
        }
        i += 1;
    }
    in_single || in_double || paren_depth > 0 || brace_depth > 0
}
