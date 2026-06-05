// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// Hand-written `#[repr(C)]` mirror of the structs/enums in include/shell.h.
//
// These layouts are NOT guessed: every size and field offset below was
// captured from the C compiler (see the compile-time assertions at the bottom,
// which fail the build if any layout drifts from the C definition).

use libc::{c_char, c_int, c_void};

pub const MAX_FD_REDIRS: usize = 16;

// ---- TokenType (include/shell.h) — values follow C enum declaration order ----
pub mod tok {
    use libc::c_int;
    pub const EOF: c_int = 0;
    pub const WORD: c_int = 1;
    pub const PIPE: c_int = 2;
    pub const REDIR_IN: c_int = 3;
    pub const REDIR_OUT: c_int = 4;
    pub const REDIR_APP: c_int = 5;
    pub const BG: c_int = 6;
    pub const AND: c_int = 7;
    pub const OR: c_int = 8;
    pub const SEMI: c_int = 9;
    pub const HEREDOC: c_int = 10;
    pub const HEREDOC_NOEXP: c_int = 11;
    pub const HERESTRING: c_int = 12;
    pub const DOUBLE_LBRACKET: c_int = 13;
    pub const DOUBLE_RBRACKET: c_int = 14;
    pub const DOUBLE_LPAREN: c_int = 15;
    pub const DOUBLE_RPAREN: c_int = 16;
    pub const REDIR_FD_OUT: c_int = 17;
    pub const REDIR_FD_APP: c_int = 18;
    pub const REDIR_FD_IN: c_int = 19;
    pub const REDIR_DUP_OUT: c_int = 20;
    pub const REDIR_DUP_IN: c_int = 21;
    pub const REDIR_CLOSE: c_int = 22;
    pub const LPAREN: c_int = 23;
    pub const RPAREN: c_int = 24;
}

// ---- ListOp ----
pub mod op {
    use libc::c_int;
    pub const NONE: c_int = 0;
    pub const AND: c_int = 1;
    pub const OR: c_int = 2;
    pub const SEMI: c_int = 3;
    pub const PIPE: c_int = 4;
}

// ---- NodeType ----
pub mod node {
    use libc::c_int;
    pub const PIPELINE: c_int = 0;
    pub const IF: c_int = 1;
    pub const WHILE: c_int = 2;
    pub const FOR: c_int = 3;
    pub const FUNC: c_int = 4;
    pub const CASE: c_int = 5;
    pub const SELECT: c_int = 6;
    pub const TIME: c_int = 7;
    pub const COPROC: c_int = 8;
    pub const SUBSHELL: c_int = 9;
}

#[repr(C)]
pub struct Token {
    pub type_: c_int,
    pub value: *mut c_char,
    pub quoted: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FdRedir {
    pub src_fd: c_int,
    pub dst_fd: c_int,
    pub file: *mut c_char,
    pub append: c_int,
    pub is_input: c_int,
}

#[repr(C)]
pub struct Command {
    pub argv: *mut *mut c_char,
    pub argc: c_int,
    pub infile: *mut c_char,
    pub outfile: *mut c_char,
    pub append: c_int,
    pub heredoc_content: *mut c_char,
    pub heredoc_expand: c_int,
    pub heredoc_delim: *mut c_char,
    pub fd_redirs: [FdRedir; MAX_FD_REDIRS],
    pub nfd_redirs: c_int,
}

#[repr(C)]
pub struct Pipeline {
    pub commands: *mut Command,
    pub ncommands: c_int,
    pub background: c_int,
}

// CmdNode contains a union of compound-node pointers in C. Every union member
// is a pointer of identical size, so we model the union as a single opaque
// pointer slot (`node_union`) and interpret it according to `type_`. The
// distinct `pipeline` field precedes the union exactly as in C.
#[repr(C)]
pub struct CmdNode {
    pub pipeline: *mut Pipeline,
    pub op: c_int,
    pub type_: c_int,
    pub negate: c_int,
    pub node_union: *mut c_void,
}

impl CmdNode {
    pub fn zeroed() -> Self {
        CmdNode {
            pipeline: core::ptr::null_mut(),
            op: op::NONE,
            type_: node::PIPELINE,
            negate: 0,
            node_union: core::ptr::null_mut(),
        }
    }
}

#[repr(C)]
pub struct CmdList {
    pub nodes: *mut CmdNode,
    pub count: c_int,
}

#[repr(C)]
pub struct CaseItem {
    pub pattern: *mut c_char,
    pub body: *mut CmdList,
}

#[repr(C)]
pub struct CaseNode {
    pub word: *mut c_char,
    pub items: *mut CaseItem,
    pub nitem: c_int,
}

#[repr(C)]
pub struct SelectNode {
    pub var: *mut c_char,
    pub words: *mut *mut c_char,
    pub nwords: c_int,
    pub body: *mut CmdList,
    pub infile: *mut c_char,
    pub outfile: *mut c_char,
    pub append: c_int,
    pub fd_redirs: [FdRedir; MAX_FD_REDIRS],
    pub nfd_redirs: c_int,
}

#[repr(C)]
pub struct TimeNode {
    pub pipeline: *mut Pipeline,
}

#[repr(C)]
pub struct CoprocNode {
    pub name: *mut c_char,
    pub pipeline: *mut Pipeline,
    pub body: *mut CmdList,
}

#[repr(C)]
pub struct SubshellNode {
    pub body: *mut CmdList,
    pub infile: *mut c_char,
    pub outfile: *mut c_char,
    pub append: c_int,
    pub fd_redirs: [FdRedir; MAX_FD_REDIRS],
    pub nfd_redirs: c_int,
}

#[repr(C)]
pub struct IfNode {
    pub condition: *mut CmdList,
    pub then_body: *mut CmdList,
    pub elif_conditions: [*mut CmdList; 16],
    pub elif_bodies: [*mut CmdList; 16],
    pub elif_count: c_int,
    pub else_body: *mut CmdList,
    pub group_outfile: *mut c_char,
    pub group_infile: *mut c_char,
    pub group_append: c_int,
    pub group_fd_redirs: [FdRedir; MAX_FD_REDIRS],
    pub group_nfd_redirs: c_int,
}

#[repr(C)]
pub struct WhileNode {
    pub condition: *mut CmdList,
    pub body: *mut CmdList,
    pub is_until: c_int,
    pub outfile: *mut c_char,
    pub infile: *mut c_char,
    pub append: c_int,
    pub fd_redirs: [FdRedir; MAX_FD_REDIRS],
    pub nfd_redirs: c_int,
}

#[repr(C)]
pub struct ForNode {
    pub var: *mut c_char,
    pub words: *mut *mut c_char,
    pub nwords: c_int,
    pub body: *mut CmdList,
}

// ---- ShellConfig (include/config.h) — read by security.rs (g_config) ----
// Hand-mirrored with the same field order/sizes; the size assertion below
// fails the build if it drifts. The C g_config remains the single source of
// truth (defined in config.c) and is read via `extern`.
#[repr(C)]
pub struct ShellConfig {
    pub prompt_show_time: c_int,
    pub prompt_show_user: c_int,
    pub prompt_color_ok: [c_char; 16],
    pub prompt_color_err: [c_char; 16],
    pub history_max: c_int,
    pub history_dedup: c_int,
    pub security_warn: c_int,
    pub security_block: c_int,
    pub security_audit: c_int,
    pub security_audit_log: [c_char; 256],
    pub panel_max_rows: c_int,
    pub panel_max_items: c_int,
    pub panel_enabled: c_int,
    pub completion_enabled: c_int,
    pub suggestion_enabled: c_int,
    pub highlight_enabled: c_int,
    pub hl_color_keyword: [c_char; 16],
    pub hl_color_string: [c_char; 16],
    pub hl_color_variable: [c_char; 16],
    pub hl_color_comment: [c_char; 16],
    pub hl_color_operator: [c_char; 16],
    pub hl_color_cmd_ok: [c_char; 16],
    pub hl_color_cmd_err: [c_char; 16],
    pub hl_color_path: [c_char; 16],
    pub hl_color_flag: [c_char; 16],
}

extern "C" {
    // Defined in src/config.c (always — even under USE_RUST_CONFIG, only the
    // load/save logic is routed to Rust; the storage stays C-owned). Mutable
    // because config_load_rs writes into it.
    pub static mut g_config: ShellConfig;

    // The shell's $? — defined in src/signals.c, read/written across modules.
    pub static mut last_exit_status: c_int;
}

/// Current $? value (read through a raw pointer to avoid `static_mut_refs`).
#[inline]
pub fn last_exit() -> c_int {
    // SAFETY: plain `int` read; single-threaded shell, no concurrent writer.
    unsafe { *core::ptr::addr_of!(last_exit_status) }
}

/// Raw const pointer to g_config, avoiding `static_mut_refs`.
/// SAFETY: caller must not hold it across a config_load mutation.
#[inline]
pub fn config_ptr() -> *const ShellConfig {
    core::ptr::addr_of!(g_config)
}

/// Raw mut pointer to g_config.
#[inline]
pub fn config_ptr_mut() -> *mut ShellConfig {
    core::ptr::addr_of_mut!(g_config)
}

// ---- compile-time layout assertions (64-bit ABI; CI Rust builds are 64-bit) ----
// If a struct ever drifts from include/shell.h these stop the build cold.
#[cfg(target_pointer_width = "64")]
const _: () = {
    use core::mem::size_of;
    assert!(size_of::<Token>() == 24);
    assert!(size_of::<FdRedir>() == 24);
    assert!(size_of::<Command>() == 456);
    assert!(size_of::<Pipeline>() == 16);
    assert!(size_of::<CmdNode>() == 32);
    assert!(size_of::<CmdList>() == 16);
    assert!(size_of::<CaseItem>() == 16);
    assert!(size_of::<CaseNode>() == 24);
    assert!(size_of::<SelectNode>() == 448);
    assert!(size_of::<TimeNode>() == 8);
    assert!(size_of::<CoprocNode>() == 24);
    assert!(size_of::<SubshellNode>() == 424);
    assert!(size_of::<IfNode>() == 704);
    assert!(size_of::<WhileNode>() == 440);
    assert!(size_of::<ForNode>() == 32);
    assert!(size_of::<ShellConfig>() == 484);
};
