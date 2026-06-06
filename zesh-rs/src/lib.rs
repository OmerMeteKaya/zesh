// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// zesh-rs — a Rust reimplementation of the list parser (`parse_list`) and the
// parameter-expansion subset of `expand_word`, compiled to a staticlib that
// links into the existing C `zesh` binary.
//
// The C implementation is preserved untouched behind `#ifndef USE_RUST_*`
// guards and remains the default. This crate is selected only when the binary
// is built with USE_RUST=1.
//
// Design notes
// ------------
// * All memory handed to / taken from C uses the C heap (libc malloc/calloc/
//   realloc/free/strdup) so that ownership can pass freely across the FFI
//   boundary and the C `free()`-based teardown paths keep working.
// * The list parser owns the recursive list/compound structure (the part where
//   leak-free error paths matter) and delegates individual pipeline parsing to
//   the proven C `parse()` for exact behavioural parity.
// * Every `#[no_mangle] extern "C"` entry point wraps its body in
//   `catch_unwind` so a Rust panic can never unwind into C — `panic = "abort"`
//   in Cargo.toml is the belt-and-braces backstop.

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]

pub mod ffi;
pub mod parser;
pub mod expand;
pub mod lexer;
pub mod alias;
pub mod security;
pub mod config;
pub mod rc;
pub mod shell;

pub use ffi::*;
