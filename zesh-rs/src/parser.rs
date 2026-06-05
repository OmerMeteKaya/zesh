// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ömer Mete Kaya
//
// Rust port of src/parser.c's list parser. This owns the recursive list /
// compound-command structure — the part where leak-free error paths were the
// source of the AFL++ crash reports — and delegates each individual pipeline
// to the proven C `parse()` for exact behavioural parity (which also keeps the
// existing heredoc / redirection / `[[ ]]` / `(( ))` handling untouched).
//
// Allocation policy: every node, list and string is allocated on the C heap
// (libc malloc/calloc/realloc/strdup) so cmdlist_free_rs() — and any C code
// that frees these structures — can interchangeably free them.

use crate::ffi::*;
use core::ptr;
use libc::{c_char, c_int, c_void};

extern "C" {
    // Provided by src/parser.c (kept compiled in both modes).
    fn parse(toks: *mut Token, ntokens: c_int) -> *mut Pipeline;
    fn pipeline_free(p: *mut Pipeline);
    // Provided by src/expand.c.
    fn func_define(name: *const c_char, body: *mut CmdList);
}

// ------------------------------------------------------------------ //
//  small helpers                                                       //
// ------------------------------------------------------------------ //

#[inline]
unsafe fn at(toks: *mut Token, i: c_int) -> *mut Token {
    toks.add(i as usize)
}

#[inline]
unsafe fn ttype(toks: *mut Token, i: c_int) -> c_int {
    (*at(toks, i)).type_
}

#[inline]
unsafe fn tval(toks: *mut Token, i: c_int) -> *mut c_char {
    (*at(toks, i)).value
}

/// strcmp(p, lit)==0, where `lit` MUST be a NUL-terminated byte literal.
unsafe fn ceq(p: *const c_char, lit: &[u8]) -> bool {
    if p.is_null() {
        return false;
    }
    libc::strcmp(p, lit.as_ptr() as *const c_char) == 0
}

unsafe fn is_keyword(t: *mut Token, kw: &[u8]) -> bool {
    (*t).type_ == tok::WORD && !(*t).value.is_null() && (*t).quoted == 0 && ceq((*t).value, kw)
}

unsafe fn compound_delta(t: *mut Token) -> c_int {
    if (*t).type_ != tok::WORD || (*t).value.is_null() || (*t).quoted != 0 {
        return 0;
    }
    let v = (*t).value;
    if ceq(v, b"if\0")
        || ceq(v, b"while\0")
        || ceq(v, b"until\0")
        || ceq(v, b"for\0")
        || ceq(v, b"case\0")
        || ceq(v, b"select\0")
        || ceq(v, b"{\0")
    {
        return 1;
    }
    if ceq(v, b"fi\0") || ceq(v, b"done\0") || ceq(v, b"esac\0") || ceq(v, b"}\0") {
        return -1;
    }
    0
}

unsafe fn find_closing(toks: *mut Token, ntokens: c_int, start: c_int, close_kw: &[u8]) -> c_int {
    let mut depth = 1;
    let mut i = start;
    while i < ntokens {
        depth += compound_delta(at(toks, i));
        let t = at(toks, i);
        if depth <= 0 && (*t).type_ == tok::WORD && !(*t).value.is_null() && ceq((*t).value, close_kw)
        {
            return i;
        }
        i += 1;
    }
    -1
}

unsafe fn find_keyword_d0(toks: *mut Token, ntokens: c_int, start: c_int, kw: &[u8]) -> c_int {
    let mut depth = 0;
    let mut i = start;
    while i < ntokens {
        depth += compound_delta(at(toks, i));
        let t = at(toks, i);
        if depth == 0 && (*t).type_ == tok::WORD && !(*t).value.is_null() && ceq((*t).value, kw) {
            return i;
        }
        i += 1;
    }
    -1
}

// ---- C-heap allocation helpers ----

unsafe fn calloc1<T>() -> *mut T {
    libc::calloc(1, core::mem::size_of::<T>()) as *mut T
}

unsafe fn dup(p: *const c_char) -> *mut c_char {
    libc::strdup(p)
}

unsafe fn dup_or_empty(p: *const c_char) -> *mut c_char {
    if p.is_null() {
        libc::strdup(b"\0".as_ptr() as *const c_char)
    } else {
        libc::strdup(p)
    }
}

/// Write a message to stderr (fd 2), matching the C `fprintf(stderr, ...)`.
unsafe fn estr(msg: &str) {
    libc::write(2, msg.as_ptr() as *const c_void, msg.len());
}

// ------------------------------------------------------------------ //
//  CmdList allocation helpers (mirror src/parser.c)                    //
// ------------------------------------------------------------------ //

unsafe fn cmdlist_new() -> *mut CmdList {
    let l = libc::malloc(core::mem::size_of::<CmdList>()) as *mut CmdList;
    if l.is_null() {
        return ptr::null_mut();
    }
    let nodes = libc::malloc(4 * core::mem::size_of::<CmdNode>()) as *mut CmdNode;
    if nodes.is_null() {
        libc::free(l as *mut c_void);
        return ptr::null_mut();
    }
    (*l).nodes = nodes;
    (*l).count = 0;
    l
}

unsafe fn cmdlist_push(list: *mut CmdList, capacity: &mut c_int, node: CmdNode) -> bool {
    if (*list).count >= *capacity {
        *capacity *= 2;
        let tmp = libc::realloc(
            (*list).nodes as *mut c_void,
            (*capacity as usize) * core::mem::size_of::<CmdNode>(),
        ) as *mut CmdNode;
        if tmp.is_null() {
            return false;
        }
        (*list).nodes = tmp;
    }
    *(*list).nodes.add((*list).count as usize) = node;
    (*list).count += 1;
    true
}

// ------------------------------------------------------------------ //
//  cmdlist_free_rs — recursively free every node type (mirror C)       //
// ------------------------------------------------------------------ //

unsafe fn cmdlist_free_impl(list: *mut CmdList) {
    if list.is_null() {
        return;
    }
    let count = (*list).count;
    for idx in 0..count {
        let n = &mut *(*list).nodes.add(idx as usize);
        match n.type_ {
            x if x == node::PIPELINE => pipeline_free(n.pipeline),
            x if x == node::FUNC => { /* body stored via func_define, not here */ }
            x if x == node::IF && !n.node_union.is_null() => {
                let inn = n.node_union as *mut IfNode;
                cmdlist_free_impl((*inn).condition);
                cmdlist_free_impl((*inn).then_body);
                for e in 0..(*inn).elif_count as usize {
                    cmdlist_free_impl((*inn).elif_conditions[e]);
                    cmdlist_free_impl((*inn).elif_bodies[e]);
                }
                cmdlist_free_impl((*inn).else_body);
                libc::free((*inn).group_outfile as *mut c_void);
                libc::free((*inn).group_infile as *mut c_void);
                for fi in 0..(*inn).group_nfd_redirs as usize {
                    libc::free((*inn).group_fd_redirs[fi].file as *mut c_void);
                }
                libc::free(inn as *mut c_void);
            }
            x if x == node::WHILE && !n.node_union.is_null() => {
                let wn = n.node_union as *mut WhileNode;
                cmdlist_free_impl((*wn).condition);
                cmdlist_free_impl((*wn).body);
                libc::free((*wn).outfile as *mut c_void);
                libc::free((*wn).infile as *mut c_void);
                for ri in 0..(*wn).nfd_redirs as usize {
                    libc::free((*wn).fd_redirs[ri].file as *mut c_void);
                }
                libc::free(wn as *mut c_void);
            }
            x if x == node::FOR && !n.node_union.is_null() => {
                let fnp = n.node_union as *mut ForNode;
                libc::free((*fnp).var as *mut c_void);
                for w in 0..(*fnp).nwords as usize {
                    libc::free(*(*fnp).words.add(w) as *mut c_void);
                }
                libc::free((*fnp).words as *mut c_void);
                cmdlist_free_impl((*fnp).body);
                libc::free(fnp as *mut c_void);
            }
            x if x == node::COPROC && !n.node_union.is_null() => {
                let cn = n.node_union as *mut CoprocNode;
                pipeline_free((*cn).pipeline);
                cmdlist_free_impl((*cn).body);
                libc::free((*cn).name as *mut c_void);
                libc::free(cn as *mut c_void);
            }
            x if x == node::SELECT && !n.node_union.is_null() => {
                let sn = n.node_union as *mut SelectNode;
                libc::free((*sn).var as *mut c_void);
                for w in 0..(*sn).nwords as usize {
                    libc::free(*(*sn).words.add(w) as *mut c_void);
                }
                libc::free((*sn).words as *mut c_void);
                cmdlist_free_impl((*sn).body);
                libc::free((*sn).infile as *mut c_void);
                libc::free((*sn).outfile as *mut c_void);
                for ri in 0..(*sn).nfd_redirs as usize {
                    libc::free((*sn).fd_redirs[ri].file as *mut c_void);
                }
                libc::free(sn as *mut c_void);
            }
            x if x == node::TIME && !n.node_union.is_null() => {
                let tn = n.node_union as *mut TimeNode;
                pipeline_free((*tn).pipeline);
                libc::free(tn as *mut c_void);
            }
            x if x == node::CASE && !n.node_union.is_null() => {
                let cn = n.node_union as *mut CaseNode;
                libc::free((*cn).word as *mut c_void);
                for ci in 0..(*cn).nitem as usize {
                    libc::free((*(*cn).items.add(ci)).pattern as *mut c_void);
                    cmdlist_free_impl((*(*cn).items.add(ci)).body);
                }
                libc::free((*cn).items as *mut c_void);
                libc::free(cn as *mut c_void);
            }
            x if x == node::SUBSHELL && !n.node_union.is_null() => {
                let sn = n.node_union as *mut SubshellNode;
                cmdlist_free_impl((*sn).body);
                libc::free((*sn).infile as *mut c_void);
                libc::free((*sn).outfile as *mut c_void);
                for fi in 0..(*sn).nfd_redirs as usize {
                    libc::free((*sn).fd_redirs[fi].file as *mut c_void);
                }
                libc::free(sn as *mut c_void);
            }
            _ => {}
        }
    }
    libc::free((*list).nodes as *mut c_void);
    libc::free(list as *mut c_void);
}

// ------------------------------------------------------------------ //
//  sub-parsers                                                         //
// ------------------------------------------------------------------ //

const KW_FI: c_int = 1;
const KW_ELIF: c_int = 2;
const KW_ELSE: c_int = 3;

unsafe fn parse_if(toks: *mut Token, ntokens: c_int) -> (*mut IfNode, c_int) {
    let mut i = 1;
    let node = calloc1::<IfNode>();
    if node.is_null() {
        return (ptr::null_mut(), 0);
    }

    let then_pos = find_keyword_d0(toks, ntokens, i, b"then\0");
    if then_pos < 0 {
        estr("zesh: syntax error: expected 'then'\n");
        libc::free(node as *mut c_void);
        return (ptr::null_mut(), 0);
    }
    (*node).condition = parse_list_internal(at(toks, i), then_pos - i);
    i = then_pos + 1;

    let mut then_done = false;

    'outer: while i < ntokens {
        let mut found = -1;
        let mut found_kw = 0;
        let mut depth = 0;
        let mut j = i;
        while j < ntokens {
            if is_keyword(at(toks, j), b"if\0") {
                depth += 1;
                j += 1;
                continue;
            }
            if is_keyword(at(toks, j), b"fi\0") {
                if depth > 0 {
                    depth -= 1;
                    j += 1;
                    continue;
                }
                found = j;
                found_kw = KW_FI;
                break;
            }
            if depth > 0 {
                j += 1;
                continue;
            }
            if is_keyword(at(toks, j), b"elif\0") {
                found = j;
                found_kw = KW_ELIF;
                break;
            }
            if is_keyword(at(toks, j), b"else\0") {
                found = j;
                found_kw = KW_ELSE;
                break;
            }
            j += 1;
        }

        if found < 0 {
            estr("zesh: syntax error: expected 'fi'\n");
            return error_if(node);
        }

        if !then_done {
            (*node).then_body = parse_list_internal(at(toks, i), found - i);
            then_done = true;
        }

        if found_kw == KW_FI {
            i = found + 1;
            break;
        }

        if found_kw == KW_ELSE {
            i = found + 1;
            let mut depth2 = 0;
            let mut fi_pos = -1;
            let mut j2 = i;
            while j2 < ntokens {
                if is_keyword(at(toks, j2), b"if\0") {
                    depth2 += 1;
                    j2 += 1;
                    continue;
                }
                if is_keyword(at(toks, j2), b"fi\0") {
                    if depth2 > 0 {
                        depth2 -= 1;
                        j2 += 1;
                        continue;
                    }
                    fi_pos = j2;
                    break;
                }
                j2 += 1;
            }
            if fi_pos < 0 {
                estr("zesh: syntax error: expected 'fi'\n");
                return error_if(node);
            }
            (*node).else_body = parse_list_internal(at(toks, i), fi_pos - i);
            i = fi_pos + 1;
            break;
        }

        // KW_ELIF
        if (*node).elif_count >= 16 {
            estr("zesh: too many elif branches\n");
            return error_if(node);
        }
        let ei = (*node).elif_count as usize;
        i = found + 1;
        let ethen = find_keyword_d0(toks, ntokens, i, b"then\0");
        if ethen < 0 {
            estr("zesh: syntax error: expected 'then'\n");
            return error_if(node);
        }
        (*node).elif_conditions[ei] = parse_list_internal(at(toks, i), ethen - i);
        i = ethen + 1;
        (*node).elif_count += 1;

        let mut found2 = -1;
        let mut d2 = 0;
        let mut j3 = i;
        while j3 < ntokens {
            if is_keyword(at(toks, j3), b"if\0") {
                d2 += 1;
                j3 += 1;
                continue;
            }
            if is_keyword(at(toks, j3), b"fi\0") {
                if d2 > 0 {
                    d2 -= 1;
                    j3 += 1;
                    continue;
                }
                found2 = j3;
                break;
            }
            if d2 > 0 {
                j3 += 1;
                continue;
            }
            if is_keyword(at(toks, j3), b"elif\0") {
                found2 = j3;
                break;
            }
            if is_keyword(at(toks, j3), b"else\0") {
                found2 = j3;
                break;
            }
            j3 += 1;
        }
        if found2 < 0 {
            estr("zesh: syntax error: expected 'fi'\n");
            return error_if(node);
        }
        (*node).elif_bodies[ei] = parse_list_internal(at(toks, i), found2 - i);
        i = found2;
        continue 'outer;
    }

    (node, i)
}

unsafe fn error_if(node: *mut IfNode) -> (*mut IfNode, c_int) {
    cmdlist_free_impl((*node).condition);
    cmdlist_free_impl((*node).then_body);
    for e in 0..(*node).elif_count as usize {
        cmdlist_free_impl((*node).elif_conditions[e]);
        cmdlist_free_impl((*node).elif_bodies[e]);
    }
    cmdlist_free_impl((*node).else_body);
    libc::free(node as *mut c_void);
    (ptr::null_mut(), 0)
}

unsafe fn parse_while(toks: *mut Token, ntokens: c_int, is_until: c_int) -> (*mut WhileNode, c_int) {
    let mut i = 1;
    let node = calloc1::<WhileNode>();
    if node.is_null() {
        return (ptr::null_mut(), 0);
    }
    (*node).is_until = is_until;

    let do_pos = find_keyword_d0(toks, ntokens, i, b"do\0");
    if do_pos < 0 {
        estr("zesh: syntax error: expected 'do'\n");
        libc::free(node as *mut c_void);
        return (ptr::null_mut(), 0);
    }
    (*node).condition = parse_list_internal(at(toks, i), do_pos - i);
    i = do_pos + 1;

    let done_pos = find_closing(toks, ntokens, i, b"done\0");
    if done_pos < 0 {
        estr("zesh: syntax error: expected 'done'\n");
        cmdlist_free_impl((*node).condition);
        libc::free(node as *mut c_void);
        return (ptr::null_mut(), 0);
    }
    (*node).body = parse_list_internal(at(toks, i), done_pos - i);
    i = done_pos + 1;

    (node, i)
}

unsafe fn parse_case(toks: *mut Token, ntokens: c_int) -> (*mut CaseNode, c_int) {
    let mut i = 1;
    let node = calloc1::<CaseNode>();
    if node.is_null() {
        return (ptr::null_mut(), 0);
    }

    if i >= ntokens || tval(toks, i).is_null() {
        estr("zesh: syntax error: expected word after 'case'\n");
        libc::free(node as *mut c_void);
        return (ptr::null_mut(), 0);
    }
    (*node).word = dup(tval(toks, i));
    i += 1;

    if i >= ntokens || !is_keyword(at(toks, i), b"in\0") {
        estr("zesh: syntax error: expected 'in'\n");
        libc::free((*node).word as *mut c_void);
        libc::free(node as *mut c_void);
        return (ptr::null_mut(), 0);
    }
    i += 1;

    let mut items_cap: usize = 8;
    (*node).items = libc::malloc(items_cap * core::mem::size_of::<CaseItem>()) as *mut CaseItem;
    (*node).nitem = 0;

    while i < ntokens {
        while i < ntokens && ttype(toks, i) == tok::SEMI {
            i += 1;
        }
        if i >= ntokens || is_keyword(at(toks, i), b"esac\0") {
            i += 1;
            break;
        }
        if i >= ntokens || tval(toks, i).is_null() {
            break;
        }

        let pattern = dup(tval(toks, i));
        i += 1;

        if i < ntokens && !tval(toks, i).is_null() && ceq(tval(toks, i), b")\0") {
            i += 1;
        }

        let body_start = i;
        let mut body_end = i;
        let mut depth = 0;
        while body_end < ntokens {
            if is_keyword(at(toks, body_end), b"case\0") {
                depth += 1;
            }
            if is_keyword(at(toks, body_end), b"esac\0") {
                if depth > 0 {
                    depth -= 1;
                } else {
                    break;
                }
            }
            if ttype(toks, body_end) == tok::SEMI
                && body_end + 1 < ntokens
                && ttype(toks, body_end + 1) == tok::SEMI
            {
                break;
            }
            body_end += 1;
        }

        let body = parse_list_internal(at(toks, body_start), body_end - body_start);

        if body_end < ntokens
            && ttype(toks, body_end) == tok::SEMI
            && body_end + 1 < ntokens
            && ttype(toks, body_end + 1) == tok::SEMI
        {
            body_end += 2;
        }
        i = body_end;

        if (*node).nitem as usize >= items_cap {
            items_cap *= 2;
            let tmp = libc::realloc(
                (*node).items as *mut c_void,
                items_cap * core::mem::size_of::<CaseItem>(),
            ) as *mut CaseItem;
            if tmp.is_null() {
                libc::free(pattern as *mut c_void);
                cmdlist_free_impl(body);
                return error_case(node);
            }
            (*node).items = tmp;
        }
        let slot = (*node).items.add((*node).nitem as usize);
        (*slot).pattern = pattern;
        (*slot).body = body;
        (*node).nitem += 1;
    }

    (node, i)
}

unsafe fn error_case(node: *mut CaseNode) -> (*mut CaseNode, c_int) {
    libc::free((*node).word as *mut c_void);
    for ci in 0..(*node).nitem as usize {
        libc::free((*(*node).items.add(ci)).pattern as *mut c_void);
        cmdlist_free_impl((*(*node).items.add(ci)).body);
    }
    libc::free((*node).items as *mut c_void);
    libc::free(node as *mut c_void);
    (ptr::null_mut(), 0)
}

unsafe fn parse_for(toks: *mut Token, ntokens: c_int) -> (*mut ForNode, c_int) {
    let mut i = 1;
    let node = calloc1::<ForNode>();
    if node.is_null() {
        return (ptr::null_mut(), 0);
    }

    if i >= ntokens || ttype(toks, i) != tok::WORD || tval(toks, i).is_null() {
        estr("zesh: syntax error: expected variable after 'for'\n");
        libc::free(node as *mut c_void);
        return (ptr::null_mut(), 0);
    }
    (*node).var = dup(tval(toks, i));
    i += 1;

    let mut words_cap: usize = 8;
    (*node).words = libc::malloc(words_cap * core::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
    (*node).nwords = 0;

    if i < ntokens && is_keyword(at(toks, i), b"in\0") {
        i += 1;
        while i < ntokens
            && !is_keyword(at(toks, i), b"do\0")
            && ttype(toks, i) != tok::SEMI
        {
            if tval(toks, i).is_null() {
                break;
            }
            if (*node).nwords as usize >= words_cap {
                words_cap *= 2;
                let tmp = libc::realloc(
                    (*node).words as *mut c_void,
                    words_cap * core::mem::size_of::<*mut c_char>(),
                ) as *mut *mut c_char;
                if tmp.is_null() {
                    return error_for(node);
                }
                (*node).words = tmp;
            }
            *(*node).words.add((*node).nwords as usize) = dup(tval(toks, i));
            (*node).nwords += 1;
            i += 1;
        }
    }
    if i < ntokens && ttype(toks, i) == tok::SEMI {
        i += 1;
    }

    if i >= ntokens || !is_keyword(at(toks, i), b"do\0") {
        estr("zesh: syntax error: expected 'do'\n");
        return error_for(node);
    }
    i += 1;

    let done_pos = find_closing(toks, ntokens, i, b"done\0");
    if done_pos < 0 {
        estr("zesh: syntax error: expected 'done'\n");
        return error_for(node);
    }
    (*node).body = parse_list_internal(at(toks, i), done_pos - i);
    i = done_pos + 1;

    (node, i)
}

unsafe fn error_for(node: *mut ForNode) -> (*mut ForNode, c_int) {
    libc::free((*node).var as *mut c_void);
    for w in 0..(*node).nwords as usize {
        libc::free(*(*node).words.add(w) as *mut c_void);
    }
    libc::free((*node).words as *mut c_void);
    cmdlist_free_impl((*node).body);
    libc::free(node as *mut c_void);
    (ptr::null_mut(), 0)
}

unsafe fn parse_select(toks: *mut Token, ntokens: c_int) -> (*mut SelectNode, c_int) {
    let mut i = 1;
    let node = calloc1::<SelectNode>();
    if node.is_null() {
        return (ptr::null_mut(), 0);
    }

    if i >= ntokens || ttype(toks, i) != tok::WORD || tval(toks, i).is_null() {
        libc::free(node as *mut c_void);
        return (ptr::null_mut(), 0);
    }
    (*node).var = dup(tval(toks, i));
    i += 1;

    let mut words_cap: usize = 8;
    (*node).words = libc::malloc(words_cap * core::mem::size_of::<*mut c_char>()) as *mut *mut c_char;
    (*node).nwords = 0;

    if i < ntokens && is_keyword(at(toks, i), b"in\0") {
        i += 1;
        while i < ntokens && !is_keyword(at(toks, i), b"do\0") && ttype(toks, i) != tok::SEMI {
            if (*node).nwords as usize >= words_cap {
                words_cap *= 2;
                let tmp = libc::realloc(
                    (*node).words as *mut c_void,
                    words_cap * core::mem::size_of::<*mut c_char>(),
                ) as *mut *mut c_char;
                if tmp.is_null() {
                    return error_select(node);
                }
                (*node).words = tmp;
            }
            *(*node).words.add((*node).nwords as usize) = dup_or_empty(tval(toks, i));
            (*node).nwords += 1;
            i += 1;
        }
    }
    if i < ntokens && ttype(toks, i) == tok::SEMI {
        i += 1;
    }
    if i >= ntokens || !is_keyword(at(toks, i), b"do\0") {
        return error_select(node);
    }
    i += 1;

    let done_pos = find_closing(toks, ntokens, i, b"done\0");
    if done_pos < 0 {
        return error_select(node);
    }
    (*node).body = parse_list_internal(at(toks, i), done_pos - i);
    i = done_pos + 1;

    // redirections after done
    while i < ntokens {
        let rtt = ttype(toks, i);
        if rtt == tok::REDIR_IN
            || rtt == tok::REDIR_OUT
            || rtt == tok::REDIR_APP
            || rtt == tok::REDIR_FD_OUT
            || rtt == tok::REDIR_FD_APP
            || rtt == tok::REDIR_FD_IN
            || rtt == tok::REDIR_DUP_OUT
            || rtt == tok::REDIR_DUP_IN
        {
            if rtt == tok::REDIR_IN && i + 1 < ntokens {
                (*node).infile = dup_or_empty(tval(toks, i + 1));
                i += 2;
            } else if rtt == tok::REDIR_OUT && i + 1 < ntokens {
                (*node).outfile = dup_or_empty(tval(toks, i + 1));
                (*node).append = 0;
                i += 2;
            } else if rtt == tok::REDIR_APP && i + 1 < ntokens {
                (*node).outfile = dup_or_empty(tval(toks, i + 1));
                (*node).append = 1;
                i += 2;
            } else if (rtt == tok::REDIR_FD_OUT
                || rtt == tok::REDIR_FD_APP
                || rtt == tok::REDIR_FD_IN
                || rtt == tok::REDIR_DUP_OUT
                || rtt == tok::REDIR_DUP_IN)
                && (*node).nfd_redirs < MAX_FD_REDIRS as c_int
            {
                let r = &mut (*node).fd_redirs[(*node).nfd_redirs as usize];
                (*node).nfd_redirs += 1;
                r.src_fd = if tval(toks, i).is_null() {
                    -1
                } else {
                    libc::atoi(tval(toks, i))
                };
                r.is_input = if rtt == tok::REDIR_FD_IN || rtt == tok::REDIR_DUP_IN {
                    1
                } else {
                    0
                };
                r.append = if rtt == tok::REDIR_FD_APP { 1 } else { 0 };
                r.dst_fd = -2;
                r.file = ptr::null_mut();
                i += 1;
                if i < ntokens && ttype(toks, i) == tok::WORD && !tval(toks, i).is_null() {
                    if ceq(tval(toks, i), b"-\0") {
                        r.dst_fd = -1;
                    } else {
                        r.file = dup(tval(toks, i));
                    }
                    i += 1;
                }
            } else {
                break;
            }
        } else {
            break;
        }
    }

    (node, i)
}

unsafe fn error_select(node: *mut SelectNode) -> (*mut SelectNode, c_int) {
    libc::free((*node).var as *mut c_void);
    for w in 0..(*node).nwords as usize {
        libc::free(*(*node).words.add(w) as *mut c_void);
    }
    libc::free((*node).words as *mut c_void);
    libc::free(node as *mut c_void);
    (ptr::null_mut(), 0)
}

// ------------------------------------------------------------------ //
//  collect_redirs — shared "redirs following a closing token" loop     //
//  Mirrors the per-construct redir loops for { } group, ( ) subshell   //
//  and while/done. Operates on caller-owned redir storage.             //
// ------------------------------------------------------------------ //

struct RedirSink<'a> {
    infile: &'a mut *mut c_char,
    outfile: &'a mut *mut c_char,
    append: &'a mut c_int,
    fds: *mut FdRedir,
    nfd: &'a mut c_int,
}

/// Returns the index just past the consumed redirections.
unsafe fn collect_redirs(toks: *mut Token, ntokens: c_int, mut j: c_int, sink: &mut RedirSink) -> c_int {
    while j < ntokens {
        let rtt = ttype(toks, j);
        if rtt == tok::REDIR_OUT || rtt == tok::REDIR_APP {
            j += 1;
            if j < ntokens && !tval(toks, j).is_null() {
                libc::free(*sink.outfile as *mut c_void);
                *sink.outfile = dup(tval(toks, j));
                *sink.append = if rtt == tok::REDIR_APP { 1 } else { 0 };
                j += 1;
            }
        } else if rtt == tok::REDIR_IN {
            j += 1;
            if j < ntokens && !tval(toks, j).is_null() {
                libc::free(*sink.infile as *mut c_void);
                *sink.infile = dup(tval(toks, j));
                j += 1;
            }
        } else if (rtt == tok::REDIR_FD_OUT
            || rtt == tok::REDIR_FD_APP
            || rtt == tok::REDIR_FD_IN
            || rtt == tok::REDIR_DUP_OUT
            || rtt == tok::REDIR_DUP_IN)
            && *sink.nfd < MAX_FD_REDIRS as c_int
            && !tval(toks, j).is_null()
        {
            let r = &mut *sink.fds.add(*sink.nfd as usize);
            *sink.nfd += 1;
            r.src_fd = libc::atoi(tval(toks, j));
            r.is_input = if rtt == tok::REDIR_DUP_IN || rtt == tok::REDIR_FD_IN {
                1
            } else {
                0
            };
            r.append = if rtt == tok::REDIR_FD_APP { 1 } else { 0 };
            r.dst_fd = -2;
            r.file = ptr::null_mut();
            j += 1;
            if j < ntokens && !tval(toks, j).is_null() {
                if ceq(tval(toks, j), b"-\0") {
                    r.dst_fd = -1;
                } else {
                    r.file = dup(tval(toks, j));
                    r.dst_fd = -2;
                }
                j += 1;
            }
        } else {
            break;
        }
    }
    j
}

// ------------------------------------------------------------------ //
//  parse_list_internal — the recursive list parser                     //
// ------------------------------------------------------------------ //

unsafe fn parse_list_internal(toks: *mut Token, ntokens: c_int) -> *mut CmdList {
    if toks.is_null() || ntokens <= 0 {
        return ptr::null_mut();
    }

    let list = cmdlist_new();
    if list.is_null() {
        return ptr::null_mut();
    }
    let mut capacity: c_int = 4;

    let mut i = 0;
    while i < ntokens {
        // skip bare semicolons / newlines
        if ttype(toks, i) == tok::SEMI {
            i += 1;
            continue;
        }

        // ! negation operator
        let mut negate = 0;
        if ttype(toks, i) == tok::WORD
            && !tval(toks, i).is_null()
            && (*at(toks, i)).quoted == 0
            && ceq(tval(toks, i), b"!\0")
            && i + 1 < ntokens
        {
            negate = 1;
            i += 1;
        }

        // ( ) subshell grouping
        if ttype(toks, i) == tok::WORD && !tval(toks, i).is_null() && ceq(tval(toks, i), b"(\0") {
            i += 1;
            let mut depth = 1;
            let body_start = i;
            let mut body_end = -1;
            let mut j = i;
            while j < ntokens {
                if ttype(toks, j) == tok::WORD && !tval(toks, j).is_null() && ceq(tval(toks, j), b"(\0")
                {
                    depth += 1;
                }
                if ttype(toks, j) == tok::WORD && !tval(toks, j).is_null() && ceq(tval(toks, j), b")\0")
                {
                    depth -= 1;
                    if depth == 0 {
                        body_end = j;
                        break;
                    }
                }
                j += 1;
            }
            if body_end < 0 {
                estr("zesh: syntax error: expected ')'\n");
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            let sbody = parse_list_internal(at(toks, body_start), body_end - body_start);
            let sn = calloc1::<SubshellNode>();
            if sn.is_null() {
                cmdlist_free_impl(sbody);
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            (*sn).body = sbody;
            let mut j = body_end + 1;

            let mut sink = RedirSink {
                infile: &mut (*sn).infile,
                outfile: &mut (*sn).outfile,
                append: &mut (*sn).append,
                fds: (*sn).fd_redirs.as_mut_ptr(),
                nfd: &mut (*sn).nfd_redirs,
            };
            j = collect_redirs(toks, ntokens, j, &mut sink);

            let mut sop = op::NONE;
            if j < ntokens {
                let tt = ttype(toks, j);
                if tt == tok::AND {
                    sop = op::AND;
                    j += 1;
                } else if tt == tok::OR {
                    sop = op::OR;
                    j += 1;
                } else if tt == tok::SEMI {
                    sop = op::SEMI;
                    j += 1;
                } else if tt == tok::PIPE {
                    sop = op::PIPE;
                    j += 1;
                }
            }
            let mut snode = CmdNode::zeroed();
            snode.type_ = node::SUBSHELL;
            snode.node_union = sn as *mut c_void;
            snode.op = sop;
            snode.negate = negate;
            if !cmdlist_push(list, &mut capacity, snode) {
                cmdlist_free_impl(sbody);
                libc::free(sn as *mut c_void);
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            i = j;
            continue;
        }

        // foo() { ... } — function definition
        if ttype(toks, i) == tok::WORD
            && !tval(toks, i).is_null()
            && !ceq(tval(toks, i), b"{\0")
            && !ceq(tval(toks, i), b"if\0")
            && !ceq(tval(toks, i), b"while\0")
            && !ceq(tval(toks, i), b"for\0")
            && !ceq(tval(toks, i), b"case\0")
            && !ceq(tval(toks, i), b"select\0")
            && i + 2 < ntokens
            && ttype(toks, i + 1) == tok::WORD
            && !tval(toks, i + 1).is_null()
            && ceq(tval(toks, i + 1), b"(\0")
            && ttype(toks, i + 2) == tok::WORD
            && !tval(toks, i + 2).is_null()
            && ceq(tval(toks, i + 2), b")\0")
            && i + 3 < ntokens
            && ttype(toks, i + 3) == tok::WORD
            && !tval(toks, i + 3).is_null()
            && ceq(tval(toks, i + 3), b"{\0")
        {
            let func_name = tval(toks, i);
            let body_start = i + 4;
            let mut depth = 1;
            let mut body_end = -1;
            let mut j = body_start;
            while j < ntokens {
                if ttype(toks, j) == tok::WORD && !tval(toks, j).is_null() {
                    if ceq(tval(toks, j), b"{\0") {
                        depth += 1;
                    } else if ceq(tval(toks, j), b"}\0") {
                        depth -= 1;
                        if depth == 0 {
                            body_end = j;
                            break;
                        }
                    }
                }
                j += 1;
            }
            if body_end < 0 {
                estr("zesh: syntax error: expected '}' in function ");
                estr(c_to_str(func_name));
                estr("\n");
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            let fbody = parse_list_internal(at(toks, body_start), body_end - body_start);
            if fbody.is_null() {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            func_define(func_name, fbody);
            i = body_end + 1;
            continue;
        }

        // { compound list } — group command (modelled as an IfNode with no condition)
        if ttype(toks, i) == tok::WORD && !tval(toks, i).is_null() && ceq(tval(toks, i), b"{\0") {
            let mut depth = 1;
            let body_start = i + 1;
            let mut body_end = -1;
            let mut j = i + 1;
            while j < ntokens {
                if ttype(toks, j) == tok::WORD && !tval(toks, j).is_null() {
                    if ceq(tval(toks, j), b"{\0") {
                        depth += 1;
                    } else if ceq(tval(toks, j), b"}\0") {
                        depth -= 1;
                        if depth == 0 {
                            body_end = j;
                            break;
                        }
                    }
                }
                j += 1;
            }
            if body_end < 0 {
                estr("zesh: syntax error: expected '}'\n");
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            let gbody = parse_list_internal(at(toks, body_start), body_end - body_start);
            let mut j = body_end + 1;
            let gn = calloc1::<IfNode>();
            if gn.is_null() {
                cmdlist_free_impl(gbody);
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            (*gn).then_body = gbody;

            let mut sink = RedirSink {
                infile: &mut (*gn).group_infile,
                outfile: &mut (*gn).group_outfile,
                append: &mut (*gn).group_append,
                fds: (*gn).group_fd_redirs.as_mut_ptr(),
                nfd: &mut (*gn).group_nfd_redirs,
            };
            j = collect_redirs(toks, ntokens, j, &mut sink);

            let mut gop = op::NONE;
            if j < ntokens {
                let tt = ttype(toks, j);
                if tt == tok::AND {
                    gop = op::AND;
                    j += 1;
                } else if tt == tok::OR {
                    gop = op::OR;
                    j += 1;
                } else if tt == tok::SEMI {
                    gop = op::SEMI;
                    j += 1;
                }
            }
            let mut gnode = CmdNode::zeroed();
            gnode.type_ = node::IF;
            gnode.node_union = gn as *mut c_void;
            gnode.op = gop;
            gnode.negate = negate;
            if !cmdlist_push(list, &mut capacity, gnode) {
                cmdlist_free_impl(gbody);
                libc::free(gn as *mut c_void);
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            i = j;
            continue;
        }

        // if ... fi
        if is_keyword(at(toks, i), b"if\0") {
            let (inn, consumed) = parse_if(at(toks, i), ntokens - i);
            if inn.is_null() {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            let mut nd = CmdNode::zeroed();
            nd.type_ = node::IF;
            nd.node_union = inn as *mut c_void;
            nd.op = op::NONE;
            nd.negate = negate;
            let mut j = i + consumed;
            if j < ntokens {
                let tt = ttype(toks, j);
                if tt == tok::AND {
                    nd.op = op::AND;
                    j += 1;
                } else if tt == tok::OR {
                    nd.op = op::OR;
                    j += 1;
                } else if tt == tok::SEMI {
                    nd.op = op::SEMI;
                    j += 1;
                }
            }
            if !cmdlist_push(list, &mut capacity, nd) {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            i = j;
            continue;
        }

        // while / until ... done
        if is_keyword(at(toks, i), b"while\0") || is_keyword(at(toks, i), b"until\0") {
            let is_until = if is_keyword(at(toks, i), b"until\0") { 1 } else { 0 };
            let (wn, consumed) = parse_while(at(toks, i), ntokens - i, is_until);
            if wn.is_null() {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            let mut nd = CmdNode::zeroed();
            nd.type_ = node::WHILE;
            nd.node_union = wn as *mut c_void;
            nd.op = op::NONE;
            nd.negate = negate;
            let mut j = i + consumed;

            let mut sink = RedirSink {
                infile: &mut (*wn).infile,
                outfile: &mut (*wn).outfile,
                append: &mut (*wn).append,
                fds: (*wn).fd_redirs.as_mut_ptr(),
                nfd: &mut (*wn).nfd_redirs,
            };
            j = collect_redirs(toks, ntokens, j, &mut sink);

            if j < ntokens {
                let tt = ttype(toks, j);
                if tt == tok::AND {
                    nd.op = op::AND;
                    j += 1;
                } else if tt == tok::OR {
                    nd.op = op::OR;
                    j += 1;
                } else if tt == tok::SEMI {
                    nd.op = op::SEMI;
                    j += 1;
                } else if tt == tok::PIPE {
                    nd.op = op::PIPE;
                    j += 1;
                }
            }
            if !cmdlist_push(list, &mut capacity, nd) {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            i = j;
            continue;
        }

        // case ... esac
        if is_keyword(at(toks, i), b"case\0") {
            let (cn, consumed) = parse_case(at(toks, i), ntokens - i);
            if cn.is_null() {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            let mut nd = CmdNode::zeroed();
            nd.type_ = node::CASE;
            nd.node_union = cn as *mut c_void;
            nd.op = op::NONE;
            nd.negate = negate;
            let mut j = i + consumed;
            if j < ntokens {
                let tt = ttype(toks, j);
                if tt == tok::AND {
                    nd.op = op::AND;
                    j += 1;
                } else if tt == tok::OR {
                    nd.op = op::OR;
                    j += 1;
                } else if tt == tok::SEMI {
                    nd.op = op::SEMI;
                    j += 1;
                }
            }
            if !cmdlist_push(list, &mut capacity, nd) {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            i = j;
            continue;
        }

        // for ... done
        if is_keyword(at(toks, i), b"for\0") {
            let (fnp, consumed) = parse_for(at(toks, i), ntokens - i);
            if fnp.is_null() {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            let mut nd = CmdNode::zeroed();
            nd.type_ = node::FOR;
            nd.node_union = fnp as *mut c_void;
            nd.op = op::NONE;
            nd.negate = negate;
            let mut j = i + consumed;
            if j < ntokens {
                let tt = ttype(toks, j);
                if tt == tok::AND {
                    nd.op = op::AND;
                    j += 1;
                } else if tt == tok::OR {
                    nd.op = op::OR;
                    j += 1;
                } else if tt == tok::SEMI {
                    nd.op = op::SEMI;
                    j += 1;
                }
            }
            if !cmdlist_push(list, &mut capacity, nd) {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            i = j;
            continue;
        }

        // select ... done
        if is_keyword(at(toks, i), b"select\0") {
            let (sn, consumed) = parse_select(at(toks, i), ntokens - i);
            if sn.is_null() {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            let mut nd = CmdNode::zeroed();
            nd.type_ = node::SELECT;
            nd.node_union = sn as *mut c_void;
            nd.op = op::NONE;
            nd.negate = negate;
            let mut j = i + consumed;
            if j < ntokens {
                let tt = ttype(toks, j);
                if tt == tok::AND {
                    nd.op = op::AND;
                    j += 1;
                } else if tt == tok::OR {
                    nd.op = op::OR;
                    j += 1;
                } else if tt == tok::SEMI {
                    nd.op = op::SEMI;
                    j += 1;
                }
            }
            if !cmdlist_push(list, &mut capacity, nd) {
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            i = j;
            continue;
        }

        // coproc [NAME] pipeline  |  coproc NAME { ... }
        if is_keyword(at(toks, i), b"coproc\0") {
            i += 1;
            let mut cp_name = dup(b"COPROC\0".as_ptr() as *const c_char);
            if i < ntokens
                && ttype(toks, i) == tok::WORD
                && !tval(toks, i).is_null()
                && i + 1 < ntokens
                && ttype(toks, i + 1) == tok::WORD
                && !tval(toks, i + 1).is_null()
            {
                libc::free(cp_name as *mut c_void);
                cp_name = dup(tval(toks, i));
                i += 1;
            }
            let mut cp_pl: *mut Pipeline = ptr::null_mut();
            let mut cp_body: *mut CmdList = ptr::null_mut();
            if i < ntokens && ttype(toks, i) == tok::WORD && !tval(toks, i).is_null() && ceq(tval(toks, i), b"{\0")
            {
                i += 1;
                let mut depth2 = 1;
                let body_start = i;
                while i < ntokens && depth2 > 0 {
                    if ttype(toks, i) == tok::WORD && !tval(toks, i).is_null() {
                        if ceq(tval(toks, i), b"{\0") {
                            depth2 += 1;
                        } else if ceq(tval(toks, i), b"}\0") {
                            depth2 -= 1;
                        }
                    }
                    if depth2 > 0 {
                        i += 1;
                    } else {
                        break;
                    }
                }
                let body_end = i;
                if i < ntokens {
                    i += 1;
                }
                if body_end > body_start {
                    cp_body = parse_list_internal(at(toks, body_start), body_end - body_start);
                }
            } else {
                let cp_start = i;
                while i < ntokens {
                    let tt = ttype(toks, i);
                    if tt == tok::AND || tt == tok::OR || tt == tok::SEMI {
                        break;
                    }
                    i += 1;
                }
                cp_pl = if i > cp_start {
                    parse(at(toks, cp_start), i - cp_start)
                } else {
                    ptr::null_mut()
                };
            }
            let cn = calloc1::<CoprocNode>();
            if cn.is_null() {
                libc::free(cp_name as *mut c_void);
                pipeline_free(cp_pl);
                cmdlist_free_impl(cp_body);
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            (*cn).name = cp_name;
            (*cn).pipeline = cp_pl;
            (*cn).body = cp_body;
            let mut cp_op = op::NONE;
            if i < ntokens {
                let tt = ttype(toks, i);
                if tt == tok::AND {
                    cp_op = op::AND;
                    i += 1;
                } else if tt == tok::OR {
                    cp_op = op::OR;
                    i += 1;
                } else if tt == tok::SEMI {
                    cp_op = op::SEMI;
                    i += 1;
                }
            }
            let mut cnode = CmdNode::zeroed();
            cnode.type_ = node::COPROC;
            cnode.node_union = cn as *mut c_void;
            cnode.op = cp_op;
            cnode.negate = negate;
            if !cmdlist_push(list, &mut capacity, cnode) {
                pipeline_free(cp_pl);
                cmdlist_free_impl(cp_body);
                libc::free(cp_name as *mut c_void);
                libc::free(cn as *mut c_void);
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            continue;
        }

        // time pipeline
        if is_keyword(at(toks, i), b"time\0") {
            i += 1;
            let seg_start2 = i;
            while i < ntokens {
                let tt = ttype(toks, i);
                if tt == tok::AND || tt == tok::OR || tt == tok::SEMI {
                    break;
                }
                i += 1;
            }
            let seg_len2 = i - seg_start2;
            let tpl = if seg_len2 > 0 {
                parse(at(toks, seg_start2), seg_len2)
            } else {
                ptr::null_mut()
            };
            let tn = calloc1::<TimeNode>();
            if tn.is_null() {
                pipeline_free(tpl);
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            (*tn).pipeline = tpl;
            let mut op2 = op::NONE;
            if i < ntokens {
                let tt = ttype(toks, i);
                if tt == tok::AND {
                    op2 = op::AND;
                    i += 1;
                } else if tt == tok::OR {
                    op2 = op::OR;
                    i += 1;
                } else if tt == tok::SEMI {
                    op2 = op::SEMI;
                    i += 1;
                }
            }
            let mut tnode = CmdNode::zeroed();
            tnode.type_ = node::TIME;
            tnode.node_union = tn as *mut c_void;
            tnode.op = op2;
            tnode.negate = negate;
            if !cmdlist_push(list, &mut capacity, tnode) {
                pipeline_free(tpl);
                libc::free(tn as *mut c_void);
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
            continue;
        }

        // ---- plain pipeline ----
        let seg_start = i;
        let mut depth_br = 0;
        let mut depth_pr = 0;

        while i < ntokens {
            let tt = ttype(toks, i);
            if tt == tok::DOUBLE_LBRACKET {
                depth_br += 1;
            } else if tt == tok::DOUBLE_RBRACKET {
                depth_br -= 1;
            } else if tt == tok::DOUBLE_LPAREN {
                depth_pr += 1;
            } else if tt == tok::DOUBLE_RPAREN {
                depth_pr -= 1;
            }

            if depth_br > 0 || depth_pr > 0 {
                i += 1;
                continue;
            }

            if tt == tok::AND || tt == tok::OR || tt == tok::SEMI {
                break;
            }

            if ttype(toks, i) == tok::WORD
                && !tval(toks, i).is_null()
                && (*at(toks, i)).quoted == 0
                && depth_br == 0
                && depth_pr == 0
                && i == seg_start
                && (ceq(tval(toks, i), b"if\0")
                    || ceq(tval(toks, i), b"while\0")
                    || ceq(tval(toks, i), b"until\0")
                    || ceq(tval(toks, i), b"for\0")
                    || ceq(tval(toks, i), b"case\0")
                    || ceq(tval(toks, i), b"select\0")
                    || ceq(tval(toks, i), b"{\0"))
            {
                break;
            }

            if tt == tok::PIPE {
                if i + 1 < ntokens {
                    let nxt = at(toks, i + 1);
                    if (*nxt).type_ == tok::LPAREN
                        || ((*nxt).type_ == tok::WORD
                            && !(*nxt).value.is_null()
                            && (*nxt).quoted == 0
                            && (ceq((*nxt).value, b"if\0")
                                || ceq((*nxt).value, b"while\0")
                                || ceq((*nxt).value, b"until\0")
                                || ceq((*nxt).value, b"for\0")
                                || ceq((*nxt).value, b"case\0")
                                || ceq((*nxt).value, b"select\0")))
                    {
                        break;
                    }
                }
                i += 1;
                continue;
            }

            i += 1;
        }

        let seg_len = i - seg_start;
        let pl = if seg_len > 0 {
            parse(at(toks, seg_start), seg_len)
        } else {
            ptr::null_mut()
        };

        let mut op_ = op::NONE;
        if i < ntokens {
            let tt = ttype(toks, i);
            if tt == tok::AND {
                op_ = op::AND;
                i += 1;
            } else if tt == tok::OR {
                op_ = op::OR;
                i += 1;
            } else if tt == tok::SEMI {
                op_ = op::SEMI;
                i += 1;
            } else if tt == tok::PIPE {
                op_ = op::PIPE;
                i += 1;
            }
        }
        if seg_len == 0 && op_ == op::NONE {
            if i < ntokens {
                estr("zesh: syntax error: unexpected token '");
                let v = tval(toks, i);
                estr(if v.is_null() { "?" } else { c_to_str(v) });
                estr("'\n");
                i += 1;
            }
            continue;
        }
        if !pl.is_null() {
            let mut nd = CmdNode::zeroed();
            nd.type_ = node::PIPELINE;
            nd.pipeline = pl;
            nd.op = op_;
            nd.negate = negate;
            if !cmdlist_push(list, &mut capacity, nd) {
                pipeline_free(pl);
                cmdlist_free_impl(list);
                return ptr::null_mut();
            }
        }
    }

    list
}

/// Borrow a C string as &str for short-lived stderr messages. Lossy on invalid
/// UTF-8 is unacceptable for a borrow, so non-UTF-8 bytes fall back to "?".
unsafe fn c_to_str<'a>(p: *const c_char) -> &'a str {
    if p.is_null() {
        return "?";
    }
    let bytes = core::slice::from_raw_parts(p as *const u8, libc::strlen(p));
    core::str::from_utf8(bytes).unwrap_or("?")
}

// ------------------------------------------------------------------ //
//  Exported FFI entry points                                           //
// ------------------------------------------------------------------ //

#[no_mangle]
pub unsafe extern "C" fn parse_list_rs(toks: *mut Token, ntokens: c_int) -> *mut CmdList {
    // SAFETY: toks points to `ntokens` valid Token structs owned by the C lexer
    // for the duration of this call; parse_list_internal only reads them and
    // calls back into C `parse()` with sub-slices of the same array.
    let res =
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| parse_list_internal(toks, ntokens)));
    res.unwrap_or(ptr::null_mut())
}

#[no_mangle]
pub unsafe extern "C" fn cmdlist_free_rs(list: *mut CmdList) {
    // SAFETY: `list` is either NULL or a CmdList previously produced by
    // parse_list_rs (all allocations on the C heap), so every free below is
    // matched. cmdlist_free_impl tolerates NULL.
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| cmdlist_free_impl(list)));
}
