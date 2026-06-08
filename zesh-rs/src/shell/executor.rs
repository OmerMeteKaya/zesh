// Execution engine

use std::collections::HashMap;
use std::path::PathBuf;
use std::os::unix::io::RawFd;

use crate::shell::types::*;
use crate::shell::vars::{VarStore, ATTR_EXPORT, ATTR_LOCAL};
use crate::shell::expand::{expand_token, expand_string, eval_arith_expr_with_vars};

#[derive(Debug, Clone, PartialEq)]
pub enum LoopControl {
    None,
    Break(usize),
    Continue(usize),
}

pub struct ExecContext {
    pub cwd: PathBuf,
    pub env: HashMap<String, String>,
    pub exit_status: i32,
    pub opt_errexit: bool,
    pub opt_xtrace: bool,
    pub opt_pipefail: bool,
    pub opt_nounset: bool,
    pub loop_control: LoopControl,
    pub returning: bool,
    pub return_value: i32,
    pub is_subshell: bool,
    pub script_file: String,
    pub lineno: u32,
    pub funcname: Vec<String>,
    pub funcname_lineno: Vec<u32>,
    pub pos_params: Vec<String>,   // $1, $2, ...
    pub getopts_idx: usize,        // internal getopts state
}

impl ExecContext {
    pub fn new() -> Self {
        ExecContext {
            cwd: std::env::current_dir().unwrap_or_else(|_| PathBuf::from("/")),
            env: std::env::vars().collect(),
            exit_status: 0,
            opt_errexit: false,
            opt_xtrace: false,
            opt_pipefail: false,
            opt_nounset: false,
            loop_control: LoopControl::None,
            returning: false,
            return_value: 0,
            is_subshell: false,
            script_file: String::new(),
            lineno: 0,
            funcname: Vec::new(),
            funcname_lineno: Vec::new(),
            pos_params: Vec::new(),
            getopts_idx: 0,
        }
    }

    pub fn new_subshell() -> Self {
        let mut ctx = Self::new();
        ctx.is_subshell = true;
        ctx
    }
}

// Execute list with borrowed vars (for command substitution context)
pub fn execute_list_with_vars(nodes: &[CmdNode], ctx: &mut ExecContext, vars: &VarStore) -> i32 {
    // We need to create a local VarStore for the child
    // Clone the vars for the subshell
    let mut local_vars = clone_vars_for_subshell(vars);
    execute_list(nodes, ctx, &mut local_vars)
}

fn clone_vars_for_subshell(vars: &VarStore) -> VarStore {
    let mut new_vars = VarStore::new();
    // Copy all vars
    for scope in &vars.scopes {
        for (k, v) in &scope.vars {
            new_vars.set_raw(k, v.value.clone(), v.attrs);
        }
    }
    // Copy arrays
    for (k, v) in &vars.arrays {
        new_vars.arrays.insert(k.clone(), v.clone());
    }
    // Copy functions
    for (k, v) in &vars.functions {
        new_vars.functions.insert(k.clone(), v.clone());
    }
    // Copy hash table
    new_vars.hash_table = vars.hash_table.clone();
    new_vars
}

pub fn execute_list(nodes: &[CmdNode], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    let mut i = 0;
    while i < nodes.len() {
        let node = &nodes[i];

        // Handle && and ||
        let status = execute_node(node, ctx, vars);
        ctx.exit_status = status;
        vars.set_raw("?", status.to_string(), 0);

        if ctx.returning || ctx.loop_control != LoopControl::None {
            return status;
        }

        match &node.op {
            NodeOp::And => {
                if status != 0 {
                    // Short circuit: skip next
                    i += 2;
                    continue;
                }
            }
            NodeOp::Or => {
                if status == 0 {
                    // Skip next
                    i += 2;
                    continue;
                }
            }
            NodeOp::End | NodeOp::Semi => {}
            NodeOp::Bg => {}
            NodeOp::Pipe | NodeOp::PipeErr => {}
        }

        if ctx.opt_errexit && status != 0 {
            return status;
        }

        i += 1;
    }
    ctx.exit_status
}

fn execute_node(node: &CmdNode, ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    ctx.lineno = node.lineno;

    // Update $LINENO
    vars.set_raw("LINENO", node.lineno.to_string(), 0);

    let status = execute_compound(&node.kind, &node.redirs, node.background, ctx, vars);

    if node.negate {
        if status == 0 { 1 } else { 0 }
    } else {
        status
    }
}

fn execute_compound(kind: &CompoundKind, redirs: &[FdRedir], background: bool, ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    match kind {
        CompoundKind::Simple(cmd) => {
            if background {
                run_in_background(cmd, redirs, ctx, vars)
            } else {
                execute_simple(cmd, redirs, ctx, vars)
            }
        }
        CompoundKind::Pipeline(cmds, pipe_err) => {
            execute_pipeline(cmds, *pipe_err, redirs, background, ctx, vars)
        }
        CompoundKind::Brace(body) => {
            // { list } with possible extra redirections
            with_redirections(redirs, ctx, vars, |ctx, vars| {
                execute_list(body, ctx, vars)
            })
        }
        CompoundKind::Subshell(body) => {
            execute_subshell(body, redirs, background, ctx, vars)
        }
        CompoundKind::If { cond, then_part, elif_parts, else_part } => {
            with_redirections(redirs, ctx, vars, |ctx, vars| {
                execute_if(cond, then_part, elif_parts, else_part.as_deref(), ctx, vars)
            })
        }
        CompoundKind::While { cond, body } => {
            with_redirections(redirs, ctx, vars, |ctx, vars| {
                execute_while(cond, body, false, ctx, vars)
            })
        }
        CompoundKind::Until { cond, body } => {
            with_redirections(redirs, ctx, vars, |ctx, vars| {
                execute_while(cond, body, true, ctx, vars)
            })
        }
        CompoundKind::For { var, words, body } => {
            with_redirections(redirs, ctx, vars, |ctx, vars| {
                execute_for(var, words, body, ctx, vars)
            })
        }
        CompoundKind::Case { word, arms } => {
            with_redirections(redirs, ctx, vars, |ctx, vars| {
                execute_case(word, arms, ctx, vars)
            })
        }
        CompoundKind::Select { var, words, body } => {
            with_redirections(redirs, ctx, vars, |ctx, vars| {
                execute_select(var, words, body, ctx, vars)
            })
        }
        CompoundKind::FuncDef { name, body } => {
            // Register function
            vars.functions.insert(name.clone(), crate::shell::vars::ShellFunction {
                name: name.clone(),
                body: body.clone(),
                defined_at_line: ctx.lineno,
                source_file: ctx.script_file.clone(),
            });
            0
        }
        CompoundKind::Time(body) => {
            execute_time(body, redirs, ctx, vars)
        }
        CompoundKind::Coproc { name, body } => {
            execute_coproc(name, body, ctx, vars)
        }
    }
}

fn with_redirections<F>(redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore, f: F) -> i32
where F: FnOnce(&mut ExecContext, &mut VarStore) -> i32
{
    if redirs.is_empty() {
        return f(ctx, vars);
    }

    // Save and apply redirections
    let saved = apply_redirections_save(redirs, ctx, vars);
    let status = f(ctx, vars);
    restore_redirections(saved);
    status
}

pub struct SavedFd {
    pub orig_fd: RawFd,
    pub saved_fd: RawFd,
}

fn apply_redirections_save(redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> Vec<SavedFd> {
    let mut saved = Vec::new();
    for redir in redirs {
        if let Some(s) = apply_one_redir_save(redir, ctx, vars) {
            saved.push(s);
        }
    }
    saved
}

fn apply_one_redir_save(redir: &FdRedir, ctx: &mut ExecContext, vars: &mut VarStore) -> Option<SavedFd> {
    let src_fd = redir.src_fd;

    // Save original fd
    // SAFETY: dup with valid fd
    let saved_fd = unsafe { libc::dup(src_fd) };

    let result = apply_one_redir_raw(redir, ctx, vars);
    if !result {
        if saved_fd >= 0 {
            // SAFETY: close valid fd
            unsafe { libc::close(saved_fd); }
        }
        return None;
    }

    if saved_fd >= 0 {
        Some(SavedFd { orig_fd: src_fd, saved_fd })
    } else {
        None
    }
}

fn apply_one_redir_raw(redir: &FdRedir, ctx: &mut ExecContext, vars: &mut VarStore) -> bool {
    let src_fd = redir.src_fd;

    if redir.dst_fd == -1 {
        // Close
        // SAFETY: close valid fd
        unsafe { libc::close(src_fd); }
        return true;
    }

    if redir.dst_fd == -3 {
        // Needs word expansion for dst fd
        if let Some(word) = &redir.dst_fd_word {
            let expanded = expand_string(word, vars, &ctx.script_file);
            if expanded == "-" {
                // SAFETY: close valid fd
                unsafe { libc::close(src_fd); }
                return true;
            }
            if let Ok(dst) = expanded.parse::<i32>() {
                // SAFETY: dup2 with valid fds
                unsafe { libc::dup2(dst, src_fd); }
                return true;
            }
        }
        return false;
    }

    if redir.dst_fd >= 0 {
        // Dup
        // SAFETY: dup2 with valid fds
        unsafe { libc::dup2(redir.dst_fd, src_fd); }
        return true;
    }

    // Open file
    if redir.is_herestr {
        // Here-string
        if let Some(word) = &redir.file {
            let expanded = expand_string(word, vars, &ctx.script_file);
            let content = expanded + "\n";
            let mut pipe_fds = [0i32; 2];
            // SAFETY: pipe() with valid ptr
            if unsafe { libc::pipe(pipe_fds.as_mut_ptr()) } != 0 {
                return false;
            }
            // Write content to write end, then close it
            // SAFETY: write with valid ptr and size
            unsafe {
                libc::write(pipe_fds[1], content.as_ptr() as *const libc::c_void, content.len());
                libc::close(pipe_fds[1]);
                if pipe_fds[0] != src_fd {
                    libc::dup2(pipe_fds[0], src_fd);
                    libc::close(pipe_fds[0]);
                }
            }
            return true;
        }
        return false;
    }

    if let Some(content) = &redir.heredoc_content {
        // Heredoc - expand variables in content
        let expanded = expand_string(content, vars, &ctx.script_file);
        let mut pipe_fds = [0i32; 2];
        // SAFETY: pipe() with valid ptr
        if unsafe { libc::pipe(pipe_fds.as_mut_ptr()) } != 0 {
            return false;
        }
        // SAFETY: write with valid ptr
        unsafe {
            libc::write(pipe_fds[1], expanded.as_ptr() as *const libc::c_void, expanded.len());
            libc::close(pipe_fds[1]);
            if pipe_fds[0] != src_fd {
                libc::dup2(pipe_fds[0], src_fd);
                libc::close(pipe_fds[0]);
            }
        }
        return true;
    }

    // Regular file
    if let Some(file) = &redir.file {
        let expanded = expand_string(file, vars, &ctx.script_file);
        let flags = if redir.is_input {
            libc::O_RDONLY
        } else if redir.append {
            libc::O_WRONLY | libc::O_CREAT | libc::O_APPEND
        } else {
            libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC
        };

        let cstr = match std::ffi::CString::new(expanded.as_str()) {
            Ok(c) => c,
            Err(_) => return false,
        };
        // SAFETY: open with valid C string path and flags
        let fd = unsafe { libc::open(cstr.as_ptr(), flags, 0o666) };
        if fd < 0 {
            eprintln!("zesh: {}: {}", expanded, std::io::Error::last_os_error());
            return false;
        }
        // SAFETY: dup2 with valid fds; only close temp fd if it differs from src_fd
        // (open() may return src_fd as the next available fd, making dup2 a no-op;
        //  closing fd==src_fd would then destroy the just-opened file descriptor)
        unsafe {
            if fd != src_fd {
                libc::dup2(fd, src_fd);
                libc::close(fd);
            }
        }
        return true;
    }

    false
}

fn restore_redirections(saved: Vec<SavedFd>) {
    for s in saved.into_iter().rev() {
        // SAFETY: dup2 and close with valid fds
        unsafe {
            libc::dup2(s.saved_fd, s.orig_fd);
            libc::close(s.saved_fd);
        }
    }
}

fn execute_simple(cmd: &Command, extra_redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    ctx.lineno = cmd.lineno;
    vars.set_raw("LINENO", cmd.lineno.to_string(), 0);

    // Combine cmd.redirs and extra_redirs
    let all_redirs: Vec<FdRedir> = cmd.redirs.iter().chain(extra_redirs.iter()).cloned().collect();

    // Apply redirections BEFORE word expansion so that ${var:?msg} errors and
    // other expansion side-effects go to the redirected fd (e.g. 2>&1 captures errors)
    let early_saved = apply_redirections_save(&all_redirs, ctx, vars);

    // Expand assignments
    let mut local_assigns: Vec<(String, String)> = Vec::new();
    for (k, v) in &cmd.assigns {
        let expanded_v = expand_string(v, vars, &ctx.script_file);
        // Apply any := assignments from expansion
        for (an, av) in crate::shell::expand::take_param_assigns() {
            vars.set(&an, av);
        }
        if crate::shell::expand::take_param_error() {
            // ${var:?err} triggered - restore and exit
            restore_redirections(early_saved);
            return 1;
        }
        local_assigns.push((k.clone(), expanded_v));
    }

    // Expand words
    let mut argv: Vec<String> = Vec::new();
    for tok in &cmd.words {
        let expanded = expand_token(tok, vars, &ctx.script_file);
        // Apply any := assignments from expansion
        for (an, av) in crate::shell::expand::take_param_assigns() {
            vars.set(&an, av);
        }
        if crate::shell::expand::take_param_error() {
            restore_redirections(early_saved);
            return 1;
        }
        argv.extend(expanded);
    }

    // Restore early redirections — builtins and exec re-apply their own,
    // and external commands apply them in the forked child
    restore_redirections(early_saved);

    // Propagate last command substitution exit status — bare assignment `VAR=$(cmd)`
    // should set $? to the cmd's exit code (bash behavior)
    let cmdsub_status = crate::shell::expand::take_cmdsub_status();

    // If no words, just apply assignments
    if argv.is_empty() {
        for (k, v) in &local_assigns {
            vars.set(k, v.clone());
        }
        // Process array literal assignments ARR=(elem1 elem2 ...)
        for (arr_name, elems) in &cmd.array_assigns {
            vars.arrays.remove(arr_name);
            for (idx, elem) in elems.iter().enumerate() {
                let expanded = expand_string(elem, vars, &ctx.script_file);
                vars.set_array_elem(arr_name, idx, expanded);
            }
        }
        // Apply redirections without command
        with_redirections(&all_redirs, ctx, vars, |_, _| 0);
        // Return cmdsub exit status if any (e.g. PERR=$(failing_cmd))
        let ret = cmdsub_status.unwrap_or(0);
        ctx.exit_status = ret;
        vars.set_raw("?", ret.to_string(), 0);
        return ret;
    }

    // For commands with words, propagate cmdsub status to $? before running cmd
    if let Some(cs_status) = cmdsub_status {
        ctx.exit_status = cs_status;
        vars.set_raw("?", cs_status.to_string(), 0);
    }

    let cmd_name = argv[0].clone();

    // Check for exec builtin (special: can do fd ops without forking)
    if cmd_name == "exec" {
        return builtin_exec(&argv[1..], &all_redirs, ctx, vars);
    }

    // Check for builtins
    if let Some(status) = try_builtin(&cmd_name, &argv[1..], &all_redirs, ctx, vars) {
        return status;
    }

    // Check for function
    if vars.functions.contains_key(&cmd_name) {
        let func_body = vars.functions[&cmd_name].body.clone();
        let func_source = vars.functions[&cmd_name].source_file.clone();
        let func_line = vars.functions[&cmd_name].defined_at_line;
        return call_function(&cmd_name, &argv[1..], &func_body, func_source, func_line, &all_redirs, ctx, vars);
    }

    // External command
    run_external(&argv, &local_assigns, &all_redirs, ctx, vars)
}

fn run_in_background(cmd: &Command, extra_redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    let all_redirs: Vec<FdRedir> = cmd.redirs.iter().chain(extra_redirs.iter()).cloned().collect();

    let mut argv: Vec<String> = Vec::new();
    for tok in &cmd.words {
        let expanded = expand_token(tok, vars, &ctx.script_file);
        argv.extend(expanded);
    }

    if argv.is_empty() {
        return 0;
    }

    // SAFETY: fork() is a valid syscall
    let pid = unsafe { libc::fork() };
    if pid < 0 {
        return 1;
    }
    if pid == 0 {
        crate::shell::signals::reset_signals_for_child();
        // Apply redirections
        // Redirect stdin to /dev/null for background jobs
        // SAFETY: open and dup2 with valid args
        unsafe {
            let devnull = libc::open(b"/dev/null\0".as_ptr() as *const libc::c_char, libc::O_RDONLY, 0);
            if devnull >= 0 {
                libc::dup2(devnull, 0);
                libc::close(devnull);
            }
        }
        for redir in &all_redirs {
            apply_one_redir_raw(redir, ctx, vars);
        }
        exec_external(&argv, &[], ctx, vars);
        // SAFETY: _exit is always safe
        unsafe { libc::_exit(127); }
    }

    // Parent
    let jid = crate::shell::jobs::jobs().add(pid, argv.join(" "));
    vars.set_raw("!", pid.to_string(), 0);
    let _ = jid;
    0
}

fn call_function(name: &str, args: &[String], body: &[CmdNode], source_file: String, def_line: u32,
                 redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {

    // Save and set positional parameters
    let saved_pos = ctx.pos_params.clone();
    ctx.pos_params = args.to_vec();

    // Push scope for locals
    vars.push_scope();

    // Set $1, $2, ... in scope
    for (i, arg) in args.iter().enumerate() {
        vars.set_raw(&(i+1).to_string(), arg.clone(), 0);
    }
    vars.set_raw("#", args.len().to_string(), 0);

    // Push funcname stack
    ctx.funcname.push(name.to_string());
    ctx.funcname_lineno.push(def_line);

    // Set $FUNCNAME and $BASH_SOURCE
    let fname_stack = ctx.funcname.join(" ");
    vars.set_raw("FUNCNAME", name.to_string(), 0);
    vars.set_raw("BASH_SOURCE", if source_file.is_empty() { ctx.script_file.clone() } else { source_file.clone() }, 0);

    let status = with_redirections(redirs, ctx, vars, |ctx, vars| {
        execute_list(body, ctx, vars)
    });

    // Pop funcname
    ctx.funcname.pop();
    ctx.funcname_lineno.pop();

    // Update FUNCNAME to parent or clear
    if let Some(fname) = ctx.funcname.last() {
        vars.set_raw("FUNCNAME", fname.clone(), 0);
    } else {
        vars.set_raw("FUNCNAME", String::new(), 0);
    }

    // Pop scope
    vars.pop_scope();

    // Restore positional parameters
    ctx.pos_params = saved_pos;

    // Handle return
    let final_status = if ctx.returning {
        let rv = ctx.return_value;
        ctx.returning = false;
        ctx.return_value = 0;
        rv
    } else {
        status
    };

    final_status
}

fn execute_if(cond: &[CmdNode], then_part: &[CmdNode], elif_parts: &[(Vec<CmdNode>, Vec<CmdNode>)],
              else_part: Option<&[CmdNode]>, ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    let cond_status = execute_list(cond, ctx, vars);
    if cond_status == 0 {
        execute_list(then_part, ctx, vars)
    } else {
        for (elif_cond, elif_body) in elif_parts {
            let s = execute_list(elif_cond, ctx, vars);
            if s == 0 {
                return execute_list(elif_body, ctx, vars);
            }
        }
        if let Some(eb) = else_part {
            execute_list(eb, ctx, vars)
        } else {
            0
        }
    }
}

fn execute_while(cond: &[CmdNode], body: &[CmdNode], until: bool, ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    let mut last_status = 0;
    loop {
        let cond_status = execute_list(cond, ctx, vars);
        let should_run = if until { cond_status != 0 } else { cond_status == 0 };

        if !should_run {
            break;
        }

        last_status = execute_list(body, ctx, vars);

        match &ctx.loop_control {
            LoopControl::Break(n) => {
                if *n <= 1 {
                    ctx.loop_control = LoopControl::None;
                } else {
                    ctx.loop_control = LoopControl::Break(n - 1);
                }
                break;
            }
            LoopControl::Continue(n) => {
                if *n <= 1 {
                    ctx.loop_control = LoopControl::None;
                } else {
                    ctx.loop_control = LoopControl::Continue(n - 1);
                    break;
                }
            }
            LoopControl::None => {}
        }

        if ctx.returning { break; }
    }
    last_status
}

fn execute_for(var: &str, words: &[Token], body: &[CmdNode], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    // Expand word list
    let mut items = Vec::new();
    for w in words {
        let expanded = expand_token(w, vars, &ctx.script_file);
        items.extend(expanded);
    }

    // If no words specified, iterate over positional params
    if words.is_empty() {
        items = ctx.pos_params.clone();
    }

    let mut last_status = 0;
    for item in items {
        vars.set(var, item);
        last_status = execute_list(body, ctx, vars);

        match &ctx.loop_control {
            LoopControl::Break(n) => {
                if *n <= 1 {
                    ctx.loop_control = LoopControl::None;
                } else {
                    ctx.loop_control = LoopControl::Break(n - 1);
                }
                break;
            }
            LoopControl::Continue(n) => {
                if *n <= 1 {
                    ctx.loop_control = LoopControl::None;
                } else {
                    ctx.loop_control = LoopControl::Continue(n - 1);
                    break;
                }
            }
            LoopControl::None => {}
        }

        if ctx.returning { break; }
    }
    last_status
}

fn execute_case(word: &Token, arms: &[CaseArm], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    let expanded_word = expand_word_single(word, vars, &ctx.script_file);

    for arm in arms {
        let mut matched = false;
        for pat in &arm.patterns {
            let pat_str = expand_word_single(pat, vars, &ctx.script_file);
            if pat_str == "*" || crate::shell::expand::glob_match(&pat_str, &expanded_word) {
                matched = true;
                break;
            }
        }
        if matched {
            return execute_list(&arm.body, ctx, vars);
        }
    }
    0
}

fn expand_word_single(tok: &Token, vars: &VarStore, script_file: &str) -> String {
    let parts = expand_token(tok, vars, script_file);
    parts.join(" ")
}

fn execute_select(var: &str, words: &[Token], body: &[CmdNode], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    // Expand word list
    let mut items: Vec<String> = Vec::new();
    for w in words {
        let expanded = expand_token(w, vars, &ctx.script_file);
        items.extend(expanded);
    }

    let mut last_status = 0;
    loop {
        // Print menu to stderr
        for (i, item) in items.iter().enumerate() {
            eprintln!("{}) {}", i + 1, item);
        }

        // Read choice from stdin
        let mut line = String::new();
        use std::io::BufRead;
        let stdin = std::io::stdin();
        match stdin.lock().read_line(&mut line) {
            Ok(0) => break, // EOF
            Ok(_) => {}
            Err(_) => break,
        }

        let line = line.trim_end_matches('\n').trim_end_matches('\r');
        vars.set_raw("REPLY", line.to_string(), 0);

        let choice = line.trim().parse::<usize>().unwrap_or(0);
        if choice >= 1 && choice <= items.len() {
            vars.set(var, items[choice - 1].clone());
        } else {
            vars.set(var, String::new());
        }

        last_status = execute_list(body, ctx, vars);

        match &ctx.loop_control {
            LoopControl::Break(_) => {
                ctx.loop_control = LoopControl::None;
                break;
            }
            LoopControl::Continue(_) => {
                ctx.loop_control = LoopControl::None;
            }
            LoopControl::None => {}
        }
        if ctx.returning { break; }
    }
    last_status
}

fn execute_pipeline(cmds: &[CmdNode], pipe_err: bool, extra_redirs: &[FdRedir], background: bool,
                    ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    if cmds.is_empty() { return 0; }
    if cmds.len() == 1 {
        return execute_node(&cmds[0], ctx, vars);
    }

    let n = cmds.len();
    let mut pipes: Vec<[RawFd; 2]> = Vec::new();

    // Create pipes
    for _ in 0..n-1 {
        let mut fds = [0i32; 2];
        // SAFETY: pipe() with valid ptr
        if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 {
            return 1;
        }
        pipes.push(fds);
    }

    let mut pids: Vec<i32> = Vec::new();

    for (i, cmd) in cmds.iter().enumerate() {
        // SAFETY: fork() is a valid syscall
        let pid = unsafe { libc::fork() };
        if pid < 0 {
            return 1;
        }
        if pid == 0 {
            // Child
            crate::shell::signals::reset_signals_for_child();

            // Setup stdin from previous pipe
            if i > 0 {
                // SAFETY: dup2 with valid fds
                unsafe {
                    libc::dup2(pipes[i-1][0], 0);
                }
            }
            // Setup stdout to next pipe
            if i < n - 1 {
                // SAFETY: dup2 with valid fds
                unsafe {
                    libc::dup2(pipes[i][1], 1);
                    if pipe_err {
                        libc::dup2(pipes[i][1], 2);
                    }
                }
            }

            // Close all pipe fds
            for p in &pipes {
                // SAFETY: close valid fds
                unsafe { libc::close(p[0]); libc::close(p[1]); }
            }

            // Apply extra redirections to last command
            if i == n - 1 {
                for redir in extra_redirs {
                    apply_one_redir_raw(redir, ctx, vars);
                }
            }

            let mut child_ctx = ExecContext::new_subshell();
            child_ctx.script_file = ctx.script_file.clone();
            child_ctx.opt_errexit = ctx.opt_errexit;
            child_ctx.funcname = ctx.funcname.clone();
            child_ctx.pos_params = ctx.pos_params.clone();

            let mut child_vars = clone_vars_for_subshell(vars);

            let status = execute_node(cmd, &mut child_ctx, &mut child_vars);
            // SAFETY: _exit is always safe
            unsafe { libc::_exit(status) };
        }
        pids.push(pid);
    }

    // Parent: close all pipe fds
    for p in &pipes {
        // SAFETY: close valid fds
        unsafe { libc::close(p[0]); libc::close(p[1]); }
    }

    // Wait for all children
    let mut last_status = 0;
    for pid in &pids {
        let mut wstatus = 0;
        // SAFETY: waitpid with valid pid
        if unsafe { libc::waitpid(*pid, &mut wstatus, 0) } > 0 {
            if libc::WIFEXITED(wstatus) {
                last_status = libc::WEXITSTATUS(wstatus);
            } else if libc::WIFSIGNALED(wstatus) {
                last_status = 128 + libc::WTERMSIG(wstatus);
            }
        }
    }

    if background {
        if let Some(&last_pid) = pids.last() {
            crate::shell::jobs::jobs().add(last_pid, "pipeline".to_string());
            vars.set_raw("!", last_pid.to_string(), 0);
        }
        return 0;
    }

    last_status
}

fn execute_subshell(body: &[CmdNode], redirs: &[FdRedir], background: bool, ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    // SAFETY: fork() is a valid syscall
    let pid = unsafe { libc::fork() };
    if pid < 0 { return 1; }

    if pid == 0 {
        // Child
        crate::shell::signals::reset_signals_for_child();

        for redir in redirs {
            apply_one_redir_raw(redir, ctx, vars);
        }

        let mut child_ctx = ExecContext::new_subshell();
        child_ctx.script_file = ctx.script_file.clone();
        child_ctx.opt_errexit = ctx.opt_errexit;
        child_ctx.pos_params = ctx.pos_params.clone();

        let mut child_vars = clone_vars_for_subshell(vars);

        let status = execute_list(body, &mut child_ctx, &mut child_vars);

        // Run EXIT trap if any
        if let Ok(trap) = crate::shell::signals::G_TRAP_EXIT.lock() {
            if let Some(action) = trap.clone() {
                drop(trap);
                crate::shell::signals::run_exit_trap(&action, &child_vars, &child_ctx.script_file);
            }
        }

        // SAFETY: _exit is always safe
        unsafe { libc::_exit(status) };
    }

    if background {
        crate::shell::jobs::jobs().add(pid, "(subshell)".to_string());
        vars.set_raw("!", pid.to_string(), 0);
        return 0;
    }

    // Wait for child
    let mut wstatus = 0;
    // SAFETY: waitpid with valid pid
    unsafe { libc::waitpid(pid, &mut wstatus, 0); }
    if libc::WIFEXITED(wstatus) {
        libc::WEXITSTATUS(wstatus)
    } else if libc::WIFSIGNALED(wstatus) {
        128 + libc::WTERMSIG(wstatus)
    } else {
        1
    }
}

fn execute_time(body: &[CmdNode], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    #[cfg(not(feature = "fuzz"))]
    let start = std::time::Instant::now();
    let mut rusage_before: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_CHILDREN, &mut rusage_before); }
    let status = with_redirections(redirs, ctx, vars, |ctx, vars| {
        execute_list(body, ctx, vars)
    });
    #[cfg(not(feature = "fuzz"))]
    let elapsed = start.elapsed();
    let mut rusage_after: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_CHILDREN, &mut rusage_after); }



    #[cfg(not(feature = "fuzz"))]
    let real_secs = elapsed.as_secs_f64();
    #[cfg(feature = "fuzz")]
    let real_secs = 0.0_f64;
    let user_secs = (rusage_after.ru_utime.tv_sec - rusage_before.ru_utime.tv_sec) as f64
        + (rusage_after.ru_utime.tv_usec - rusage_before.ru_utime.tv_usec) as f64 / 1_000_000.0;
    let sys_secs = (rusage_after.ru_stime.tv_sec - rusage_before.ru_stime.tv_sec) as f64
        + (rusage_after.ru_stime.tv_usec - rusage_before.ru_stime.tv_usec) as f64 / 1_000_000.0;

    let fmt_time = |secs: f64| -> String {
        let m = (secs / 60.0) as u64;
        let s = secs - (m as f64 * 60.0);
        format!("{}m{:.3}s", m, s)
    };

    #[cfg(feature = "fuzz")]
    {
        eprintln!("\nreal\t0m0.000s\nuser\t0m0.000s\nsys\t0m0.000s");
    }
    #[cfg(not(feature = "fuzz"))]
    {
        eprintln!("\nreal\t{}", fmt_time(real_secs));
        eprintln!("user\t{}", fmt_time(user_secs));
        eprintln!("sys\t{}", fmt_time(sys_secs));
    }

    status
}

fn execute_coproc(name: &str, body: &[CmdNode], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    // Create two pipes: pipe1 for stdin (parent writes, child reads)
    //                   pipe2 for stdout (child writes, parent reads)
    let mut pipe1 = [0i32; 2];
    let mut pipe2 = [0i32; 2];
    // SAFETY: pipe() with valid ptr
    if unsafe { libc::pipe(pipe1.as_mut_ptr()) } != 0 { return 1; }
    if unsafe { libc::pipe(pipe2.as_mut_ptr()) } != 0 {
        // SAFETY: close valid fds
        unsafe { libc::close(pipe1[0]); libc::close(pipe1[1]); }
        return 1;
    }

    // SAFETY: fork() is a valid syscall
    let pid = unsafe { libc::fork() };
    if pid < 0 { return 1; }

    if pid == 0 {
        // Child
        crate::shell::signals::reset_signals_for_child();
        // SAFETY: dup2 and close with valid fds
        unsafe {
            // stdin = pipe1 read end
            libc::dup2(pipe1[0], 0);
            libc::close(pipe1[0]);
            libc::close(pipe1[1]);
            // stdout = pipe2 write end
            libc::dup2(pipe2[1], 1);
            libc::close(pipe2[0]);
            libc::close(pipe2[1]);
        }

        let mut child_ctx = ExecContext::new_subshell();
        child_ctx.script_file = ctx.script_file.clone();
        let mut child_vars = clone_vars_for_subshell(vars);

        let status = execute_list(body, &mut child_ctx, &mut child_vars);
        // SAFETY: _exit is always safe
        unsafe { libc::_exit(status) };
    }

    // Parent
    // SAFETY: close valid fds
    unsafe {
        libc::close(pipe1[0]);
        libc::close(pipe2[1]);
    }

    // Set CPCAT[0]=pipe2[0] (read), CPCAT[1]=pipe1[1] (write)
    let arr = vars.get_array_mut(name);
    arr.insert(0, pipe2[0].to_string());
    arr.insert(1, pipe1[1].to_string());

    vars.set_raw("!", pid.to_string(), 0);
    crate::shell::jobs::jobs().add(pid, format!("coproc {}", name));

    0
}

// External command execution
fn run_external(argv: &[String], assigns: &[(String, String)], redirs: &[FdRedir],
                ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    // SAFETY: fork() is a valid syscall
    let pid = unsafe { libc::fork() };
    if pid < 0 { return 1; }

    if pid == 0 {
        crate::shell::signals::reset_signals_for_child();
        // Apply redirections in child
        for redir in redirs {
            apply_one_redir_raw(redir, ctx, vars);
        }
        exec_external(argv, assigns, ctx, vars);
        // SAFETY: _exit is always safe
        unsafe { libc::_exit(127) };
    }

    // Parent: wait
    crate::shell::signals::G_FOREGROUND_PID.store(pid, std::sync::atomic::Ordering::SeqCst);
    let mut wstatus = 0;
    // SAFETY: waitpid with valid pid
    let result = unsafe { libc::waitpid(pid, &mut wstatus, 0) };
    crate::shell::signals::G_FOREGROUND_PID.store(-1, std::sync::atomic::Ordering::SeqCst);

    if result < 0 {
        return 1;
    }

    if libc::WIFEXITED(wstatus) {
        libc::WEXITSTATUS(wstatus)
    } else if libc::WIFSIGNALED(wstatus) {
        128 + libc::WTERMSIG(wstatus)
    } else {
        1
    }
}

fn exec_external(argv: &[String], extra_assigns: &[(String, String)], ctx: &mut ExecContext, vars: &mut VarStore) {
    if argv.is_empty() { return; }

    let cmd = &argv[0];

    // Find in PATH or hash table
    let exe_path = if cmd.contains('/') {
        cmd.clone()
    } else {
        // Check hash table
        if let Some(path) = vars.hash_table.get(cmd) {
            path.clone()
        } else {
            find_in_path(cmd, vars).unwrap_or_else(|| cmd.clone())
        }
    };

    // Build C argv
    let c_args: Vec<std::ffi::CString> = argv.iter()
        .map(|s| std::ffi::CString::new(s.as_str()).unwrap_or_default())
        .collect();
    let mut c_argv: Vec<*const libc::c_char> = c_args.iter().map(|s| s.as_ptr()).collect();
    c_argv.push(std::ptr::null());

    // Build environment
    let mut env_map: HashMap<String, String> = std::env::vars().collect();
    for (k, v) in extra_assigns {
        env_map.insert(k.clone(), v.clone());
    }
    // Add exported vars
    for (k, v) in vars.all_exported() {
        env_map.insert(k, v);
    }

    let c_envs: Vec<std::ffi::CString> = env_map.iter()
        .map(|(k, v)| std::ffi::CString::new(format!("{}={}", k, v)).unwrap_or_default())
        .collect();
    let mut c_envp: Vec<*const libc::c_char> = c_envs.iter().map(|s| s.as_ptr()).collect();
    c_envp.push(std::ptr::null());

    let c_path = std::ffi::CString::new(exe_path.as_str()).unwrap_or_default();

    // SAFETY: execve with valid C string pointers
    unsafe { libc::execve(c_path.as_ptr(), c_argv.as_ptr(), c_envp.as_ptr()); }

    // execve failed
    eprintln!("zesh: {}: {}", cmd, std::io::Error::last_os_error());
}

pub fn find_in_path(cmd: &str, vars: &VarStore) -> Option<String> {
    let path = vars.get_str("PATH").unwrap_or_else(|| "/usr/local/bin:/usr/bin:/bin".to_string());
    for dir in path.split(':') {
        let full = format!("{}/{}", dir, cmd);
        if std::path::Path::new(&full).is_file() {
            // Check execute permission
            let cstr = std::ffi::CString::new(full.as_str()).ok()?;
            // SAFETY: access with valid C string
            if unsafe { libc::access(cstr.as_ptr(), libc::X_OK) } == 0 {
                return Some(full);
            }
        }
    }
    None
}

// Special exec builtin
fn builtin_exec(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    // Check if it's fd operations only (no command)
    // exec N>file, exec N<file, exec N>&M, exec N>&-

    if args.is_empty() {
        // Just redirections
        for redir in redirs {
            apply_one_redir_raw(redir, ctx, vars);
        }
        return 0;
    }

    // First, expand args to see what we have
    // Check for the pattern: exec FDWORD>&- or exec FDWORD>FILE etc
    // where FDWORD expands to a number
    let mut real_redirs: Vec<FdRedir> = redirs.to_vec();

    // Check if first arg is a pure number (fd spec) AND there are redirs
    // In that case, the arg is the fd number for the redirs
    let first_arg_expanded = expand_string(&args[0], vars, &ctx.script_file);
    let is_fd_only = !redirs.is_empty() && first_arg_expanded.trim().parse::<i32>().is_ok() && {
        // Check if ALL args are fd specs (numbers)
        args.iter().all(|a| {
            let e = expand_string(a, vars, &ctx.script_file);
            e.trim().parse::<i32>().is_ok()
        })
    };

    if is_fd_only {
        // args[0] is the fd number for the first redir
        let fd_num: i32 = first_arg_expanded.trim().parse().unwrap_or(-1);
        // Override src_fd of the redir with this fd number
        for redir in &mut real_redirs {
            if redir.src_fd == 1 || redir.src_fd == 0 {
                redir.src_fd = fd_num;
            }
        }
        for redir in &real_redirs {
            apply_one_redir_raw(redir, ctx, vars);
        }
        return 0;
    }

    // Apply fd redirections without a command
    // If there's a command, do execvp
    // Apply redirections
    for redir in redirs {
        apply_one_redir_raw(redir, ctx, vars);
    }

    // If we have a command, replace current process
    let mut argv = args.to_vec();

    // Check for -l, -a, -c flags
    let mut i = 0;
    while i < argv.len() {
        match argv[i].as_str() {
            "-l" | "-a" | "-c" => { i += 1; if i < argv.len() { i += 1; } }
            _ => break,
        }
    }

    if i < argv.len() {
        let exe = &argv[i];
        let exe_args = &argv[i..];
        let exe_path = if exe.contains('/') {
            exe.clone()
        } else {
            find_in_path(exe, vars).unwrap_or_else(|| exe.clone())
        };

        let c_args: Vec<std::ffi::CString> = exe_args.iter()
            .map(|s| std::ffi::CString::new(s.as_str()).unwrap_or_default())
            .collect();
        let mut c_argv: Vec<*const libc::c_char> = c_args.iter().map(|s| s.as_ptr()).collect();
        c_argv.push(std::ptr::null());

        let env_map: HashMap<String, String> = std::env::vars().collect();
        let c_envs: Vec<std::ffi::CString> = env_map.iter()
            .map(|(k, v)| std::ffi::CString::new(format!("{}={}", k, v)).unwrap_or_default())
            .collect();
        let mut c_envp: Vec<*const libc::c_char> = c_envs.iter().map(|s| s.as_ptr()).collect();
        c_envp.push(std::ptr::null());

        let c_path = std::ffi::CString::new(exe_path.as_str()).unwrap_or_default();
        // SAFETY: execve with valid C string pointers
        unsafe { libc::execve(c_path.as_ptr(), c_argv.as_ptr(), c_envp.as_ptr()); }
        eprintln!("zesh: exec: {}: {}", exe, std::io::Error::last_os_error());
        return 127;
    }

    0
}

fn try_builtin(name: &str, args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> Option<i32> {
    use crate::shell::builtins::*;
    let status = match name {
        "echo"      => Some(builtin_echo(args, redirs, ctx, vars)),
        "printf"    => Some(builtin_printf(args, redirs, ctx, vars)),
        "cd"        => Some(builtin_cd(args, ctx, vars)),
        "pwd"       => Some(builtin_pwd(args, redirs, ctx, vars)),
        "exit"      => Some(builtin_exit(args, ctx, vars)),
        "return"    => Some(builtin_return(args, ctx, vars)),
        "break"     => Some(builtin_break(args, ctx, vars)),
        "continue"  => Some(builtin_continue(args, ctx, vars)),
        "set"       => Some(builtin_set(args, ctx, vars)),
        "unset"     => Some(builtin_unset(args, vars)),
        "export"    => Some(builtin_export(args, vars)),
        "readonly"  => Some(builtin_readonly(args, vars)),
        "local"     => Some(builtin_local(args, vars)),
        "declare" | "typeset" => Some(builtin_declare(args, vars)),
        "read"      => Some(builtin_read(args, redirs, ctx, vars)),
        "source" | "." => Some(builtin_source(args, ctx, vars)),
        "true" | ":" => Some(0),
        "false"     => Some(1),
        "test" | "[" => Some(builtin_test(args)),
        "[[" | "]]" => Some(0), // handled in parser
        "eval"      => Some(builtin_eval(args, ctx, vars)),
        "type"      => {
            let status = builtin_type(args, vars);
            Some(with_redirections(redirs, ctx, vars, |_, _| status))
        }
        "hash"      => Some(builtin_hash(args, redirs, ctx, vars)),
        "wait"      => Some(builtin_wait(args, vars)),
        "jobs"      => Some(builtin_jobs(args, redirs, ctx, vars)),
        "kill"      => Some(builtin_kill(args)),
        "trap"      => Some(builtin_trap(args, ctx, vars)),
        "umask"     => Some(builtin_umask(args, redirs, ctx, vars)),
        "ulimit"    => Some(builtin_ulimit(args, redirs, ctx, vars)),
        "getopts"   => Some(builtin_getopts(args, ctx, vars)),
        "mapfile" | "readarray" => Some(builtin_mapfile(args, redirs, ctx, vars)),
        "select"    => None, // handled as compound
        "caller"    => Some(with_redirections(redirs, ctx, vars, |ctx, _| builtin_caller(args, ctx))),
        "compgen"   => Some(builtin_compgen(args, redirs, ctx, vars)),
        "complete"  => Some(0),
        "disown"    => Some(builtin_disown(args, vars)),
        "shift"     => Some(builtin_shift(args, ctx, vars)),
        "printf"    => Some(builtin_printf(args, redirs, ctx, vars)),
        "times"     => Some(0),
        "suspend"   => Some(0),
        "fc"        => Some(0),
        "history"   => Some(builtin_history(args, redirs, ctx, vars)),
        "dirs"      => Some(0),
        "popd"      => Some(0),
        "pushd"     => Some(0),
        "let"       => Some(builtin_let(args, vars)),
        "builtin"   => {
            if args.is_empty() {
                Some(0)
            } else {
                let new_name = &args[0];
                let new_args = &args[1..];
                try_builtin(new_name, new_args, redirs, ctx, vars)
            }
        }
        "command"   => {
            if args.is_empty() {
                Some(0)
            } else {
                // Run as external command (bypass functions)
                let new_argv: Vec<String> = args.to_vec();
                Some(run_external(&new_argv, &[], redirs, ctx, vars))
            }
        }
        _           => None,
    };
    status
}

// Run command for script mode
pub fn run_script(script: &str, script_file: &str, ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    let tokens = crate::shell::lexer::lex(script);
    let nodes = crate::shell::parser::parse(tokens);
    ctx.script_file = script_file.to_string();
    vars.set_raw("BASH_SOURCE", script_file.to_string(), 0);
    vars.set_raw("0", script_file.to_string(), 0);
    execute_list(&nodes, ctx, vars)
}

// Public helpers for builtins
pub fn apply_redirections_for_builtin(redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> Vec<SavedFd> {
    apply_redirections_save(redirs, ctx, vars)
}

pub fn restore_redirections_builtin(saved: Vec<SavedFd>) {
    restore_redirections(saved);
}
