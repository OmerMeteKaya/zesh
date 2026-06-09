// Shell builtins

use std::io::{self, Write, BufRead};
use crate::shell::types::FdRedir;
use crate::shell::executor::{ExecContext, LoopControl};
use crate::shell::vars::{VarStore, ATTR_READONLY, ATTR_INTEGER, ATTR_UPPERCASE, ATTR_LOWERCASE, ATTR_EXPORT, ATTR_LOCAL};
use crate::shell::expand::{expand_string, eval_arith_simple};

pub fn builtin_echo(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);

    let mut no_newline = false;
    let mut interpret_escapes = false;
    let mut start = 0;

    while start < args.len() {
        match args[start].as_str() {
            "-n" => { no_newline = true; start += 1; }
            "-e" => { interpret_escapes = true; start += 1; }
            "-E" => { interpret_escapes = false; start += 1; }
            _ => break,
        }
    }

    let parts: Vec<String> = args[start..].to_vec();
    let output = if interpret_escapes {
        parts.iter().map(|s| interpret_escape_seq(s)).collect::<Vec<_>>().join(" ")
    } else {
        parts.join(" ")
    };

    if no_newline {
        print!("{}", output);
    } else {
        println!("{}", output);
    }
    let _ = io::stdout().flush();

    crate::shell::executor::restore_redirections_builtin(saved);
    0
}

fn interpret_escape_seq(s: &str) -> String {
    let mut result = String::new();
    let chars: Vec<char> = s.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '\\' && i + 1 < chars.len() {
            i += 1;
            match chars[i] {
                'n'  => result.push('\n'),
                't'  => result.push('\t'),
                'r'  => result.push('\r'),
                'a'  => result.push('\x07'),
                'b'  => result.push('\x08'),
                'e' | 'E' => result.push('\x1b'),
                'f'  => result.push('\x0c'),
                'v'  => result.push('\x0b'),
                '\\' => result.push('\\'),
                '0' => {
                    let mut oct = String::new();
                    i += 1;
                    while i < chars.len() && oct.len() < 3 && chars[i] >= '0' && chars[i] <= '7' {
                        oct.push(chars[i]);
                        i += 1;
                    }
                    if let Ok(n) = u32::from_str_radix(&oct, 8) {
                        if let Some(c) = char::from_u32(n) {
                            result.push(c);
                        }
                    }
                    continue;
                }
                c    => { result.push('\\'); result.push(c); }
            }
        } else {
            result.push(chars[i]);
        }
        i += 1;
    }
    result
}

pub fn builtin_printf(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);

    if args.is_empty() {
        crate::shell::executor::restore_redirections_builtin(saved);
        return 1;
    }

    let fmt = &args[0];
    let fmt_args = &args[1..];
    let output = printf_format(fmt, fmt_args);
    print!("{}", output);
    let _ = io::stdout().flush();

    crate::shell::executor::restore_redirections_builtin(saved);
    0
}

fn printf_format(fmt: &str, args: &[String]) -> String {
    let mut result = String::new();
    let chars: Vec<char> = fmt.chars().collect();
    let mut i = 0;
    let mut arg_idx = 0;

    while i < chars.len() {
        if chars[i] == '\\' {
            i += 1;
            match chars.get(i).copied().unwrap_or('\0') {
                'n'  => { result.push('\n'); i += 1; }
                't'  => { result.push('\t'); i += 1; }
                'r'  => { result.push('\r'); i += 1; }
                '\\' => { result.push('\\'); i += 1; }
                '\0' => {}
                c    => { result.push('\\'); result.push(c); i += 1; }
            }
        } else if chars[i] == '%' {
            i += 1;
            if i >= chars.len() { break; }
            match chars[i] {
                's' => {
                    let arg = args.get(arg_idx).map(|s| s.as_str()).unwrap_or("");
                    result.push_str(arg);
                    arg_idx += 1;
                    i += 1;
                }
                'd' | 'i' => {
                    let arg = args.get(arg_idx).map(|s| s.as_str()).unwrap_or("0");
                    let n: i64 = arg.trim().parse().unwrap_or(0);
                    result.push_str(&n.to_string());
                    arg_idx += 1;
                    i += 1;
                }
                'f' => {
                    let arg = args.get(arg_idx).map(|s| s.as_str()).unwrap_or("0");
                    let n: f64 = arg.trim().parse().unwrap_or(0.0);
                    result.push_str(&format!("{:.6}", n));
                    arg_idx += 1;
                    i += 1;
                }
                '%' => {
                    result.push('%');
                    i += 1;
                }
                _ => {
                    result.push('%');
                    i += 1;
                }
            }
        } else {
            result.push(chars[i]);
            i += 1;
        }
    }
    result
}

pub fn builtin_cd(args: &[String], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    let target = if args.is_empty() {
        vars.get_str("HOME").unwrap_or_else(|| "/".to_string())
    } else if args[0] == "-" {
        vars.get_str("OLDPWD").unwrap_or_else(|| ctx.cwd.to_string_lossy().into_owned())
    } else {
        args[0].clone()
    };

    let old_pwd = ctx.cwd.to_string_lossy().into_owned();

    match std::env::set_current_dir(&target) {
        Ok(()) => {
            ctx.cwd = std::env::current_dir().unwrap_or_else(|_| std::path::PathBuf::from("/"));
            vars.set_raw("OLDPWD", old_pwd, 0);
            vars.set_raw("PWD", ctx.cwd.to_string_lossy().into_owned(), 0);
            0
        }
        Err(e) => {
            eprintln!("zesh: cd: {}: {}", target, e);
            1
        }
    }
}

pub fn builtin_pwd(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);
    println!("{}", ctx.cwd.display());
    crate::shell::executor::restore_redirections_builtin(saved);
    0
}

pub fn builtin_exit(args: &[String], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    let code = if let Some(a) = args.first() {
        a.parse().unwrap_or(ctx.exit_status)
    } else {
        ctx.exit_status
    };

    // Run EXIT trap
    if let Ok(trap) = crate::shell::signals::G_TRAP_EXIT.lock() {
        if let Some(action) = trap.clone() {
            drop(trap);
            crate::shell::signals::run_exit_trap(&action, vars, &ctx.script_file);
        }
    }

    std::process::exit(code);
}

pub fn builtin_return(args: &[String], ctx: &mut ExecContext, _vars: &mut VarStore) -> i32 {
    let code = if let Some(a) = args.first() {
        a.parse().unwrap_or(ctx.exit_status)
    } else {
        ctx.exit_status
    };
    ctx.returning = true;
    ctx.return_value = code;
    code
}

pub fn builtin_break(args: &[String], ctx: &mut ExecContext, _vars: &mut VarStore) -> i32 {
    let n = if let Some(a) = args.first() {
        a.parse().unwrap_or(1)
    } else {
        1
    };
    ctx.loop_control = LoopControl::Break(n);
    0
}

pub fn builtin_continue(args: &[String], ctx: &mut ExecContext, _vars: &mut VarStore) -> i32 {
    let n = if let Some(a) = args.first() {
        a.parse().unwrap_or(1)
    } else {
        1
    };
    ctx.loop_control = LoopControl::Continue(n);
    0
}

pub fn builtin_set(args: &[String], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    if args.is_empty() {
        // Print all variables
        for (k, v) in vars.all_vars() {
            println!("{}={}", k, v);
        }
        return 0;
    }

    // Set positional parameters or options
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--" => {
                // Set positional params
                ctx.pos_params = args[i+1..].to_vec();
                for (j, p) in ctx.pos_params.iter().enumerate() {
                    vars.set_raw(&(j+1).to_string(), p.clone(), 0);
                }
                vars.set_raw("#", ctx.pos_params.len().to_string(), 0);
                return 0;
            }
            "-e" => { ctx.opt_errexit = true; }
            "+e" => { ctx.opt_errexit = false; }
            "-x" => { ctx.opt_xtrace = true; }
            "+x" => { ctx.opt_xtrace = false; }
            "-o" => {
                i += 1;
                if i < args.len() {
                    match args[i].as_str() {
                        "errexit" => { ctx.opt_errexit = true; }
                        "pipefail" => { ctx.opt_pipefail = true; }
                        "xtrace" => { ctx.opt_xtrace = true; }
                        _ => {}
                    }
                }
            }
            s if s.starts_with('-') && s.len() > 1 => {
                // Multiple flags: -ef, -ex, etc
                for c in s[1..].chars() {
                    match c {
                        'e' => { ctx.opt_errexit = true; }
                        'x' => { ctx.opt_xtrace = true; }
                        'u' => { ctx.opt_nounset = true; }
                        _ => {}
                    }
                }
            }
            s if s.starts_with('+') && s.len() > 1 => {
                for c in s[1..].chars() {
                    match c {
                        'e' => { ctx.opt_errexit = false; }
                        'x' => { ctx.opt_xtrace = false; }
                        'u' => { ctx.opt_nounset = false; }
                        _ => {}
                    }
                }
            }
            _ => {
                // set args[i..] as positional params
                ctx.pos_params = args[i..].to_vec();
                for (j, p) in ctx.pos_params.iter().enumerate() {
                    vars.set_raw(&(j+1).to_string(), p.clone(), 0);
                }
                vars.set_raw("#", ctx.pos_params.len().to_string(), 0);
                return 0;
            }
        }
        i += 1;
    }
    0
}

pub fn builtin_unset(args: &[String], vars: &mut VarStore) -> i32 {
    let mut flags_a = false;
    let mut flags_f = false;
    for arg in args {
        match arg.as_str() {
            "-v" => {}
            "-f" => { flags_f = true; }
            "-a" => { flags_a = true; }
            name => {
                if flags_f {
                    vars.functions.remove(name);
                } else {
                    vars.unset(name);
                    if flags_a {
                        vars.arrays.remove(name);
                    }
                }
            }
        }
    }
    0
}

pub fn builtin_export(args: &[String], vars: &mut VarStore) -> i32 {
    if args.is_empty() || args[0] == "-p" {
        // Print exported vars
        for (k, v) in vars.all_vars() {
            if vars.get(k.as_str()).map(|v| v.attrs & ATTR_EXPORT != 0).unwrap_or(false) {
                println!("declare -x {}=\"{}\"", k, v);
            }
        }
        return 0;
    }
    for arg in args {
        if let Some(eq) = arg.find('=') {
            let k = &arg[..eq];
            let v = arg[eq+1..].to_string();
            vars.set_with_attrs(k, v, ATTR_EXPORT);
        } else {
            // Mark as exported
            let val = vars.get_str(arg).unwrap_or_default();
            vars.set_with_attrs(arg, val, ATTR_EXPORT);
        }
    }
    0
}

pub fn builtin_readonly(args: &[String], vars: &mut VarStore) -> i32 {
    if args.is_empty() || args[0] == "-p" {
        return 0;
    }
    for arg in args {
        if let Some(eq) = arg.find('=') {
            let k = &arg[..eq];
            let v = arg[eq+1..].to_string();
            vars.set_with_attrs(k, v, ATTR_READONLY);
        } else {
            let val = vars.get_str(arg).unwrap_or_default();
            vars.set_with_attrs(arg, val, ATTR_READONLY);
        }
    }
    0
}

pub fn builtin_local(args: &[String], vars: &mut VarStore) -> i32 {
    for arg in args {
        // Handle -n, -u, -l, -i flags
        if arg.starts_with('-') { continue; }
        if let Some(eq) = arg.find('=') {
            let k = &arg[..eq];
            let v = arg[eq+1..].to_string();
            vars.set_local(k, v);
        } else {
            vars.set_local(arg, String::new());
        }
    }
    0
}

pub fn builtin_declare(args: &[String], vars: &mut VarStore) -> i32 {
    let mut print_mode = false;
    let mut attrs_to_set: u32 = 0;
    let mut i = 0;

    while i < args.len() {
        match args[i].as_str() {
            "-p" => { print_mode = true; }
            "-r" => { attrs_to_set |= ATTR_READONLY; }
            "-i" => { attrs_to_set |= ATTR_INTEGER; }
            "-u" => { attrs_to_set |= ATTR_UPPERCASE; }
            "-l" => { attrs_to_set |= ATTR_LOWERCASE; }
            "-x" => { attrs_to_set |= ATTR_EXPORT; }
            "-a" => { /* array - handled below */ }
            "-A" => { /* assoc array */ }
            "-g" => { /* global */ }
            s if s.starts_with('-') => {
                for c in s[1..].chars() {
                    match c {
                        'r' => { attrs_to_set |= ATTR_READONLY; }
                        'i' => { attrs_to_set |= ATTR_INTEGER; }
                        'u' => { attrs_to_set |= ATTR_UPPERCASE; }
                        'l' => { attrs_to_set |= ATTR_LOWERCASE; }
                        'x' => { attrs_to_set |= ATTR_EXPORT; }
                        _ => {}
                    }
                }
            }
            _ => break,
        }
        i += 1;
    }

    if print_mode {
        // Print info about variable
        for arg in &args[i..] {
            if let Some(var) = vars.get(arg) {
                let mut flags = String::from("declare");
                if var.attrs & ATTR_READONLY != 0 { flags.push_str(" -r"); }
                if var.attrs & ATTR_INTEGER != 0 { flags.push_str(" -i"); }
                if var.attrs & ATTR_UPPERCASE != 0 { flags.push_str(" -u"); }
                if var.attrs & ATTR_LOWERCASE != 0 { flags.push_str(" -l"); }
                if var.attrs & ATTR_EXPORT != 0 { flags.push_str(" -x"); }
                println!("{} {}=\"{}\"", flags, arg, var.value);
            } else {
                eprintln!("zesh: declare: {}: not found", arg);
                return 1;
            }
        }
        return 0;
    }

    for arg in &args[i..] {
        if let Some(eq) = arg.find('=') {
            let k = &arg[..eq];
            let v = arg[eq+1..].to_string();
            vars.set_with_attrs(k, v, attrs_to_set);
        } else {
            let val = vars.get_str(arg).unwrap_or_default();
            vars.set_with_attrs(arg, val, attrs_to_set);
        }
    }
    0
}

pub fn builtin_read(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);

    let mut fd = 0i32;  // stdin
    let mut var_names: Vec<String> = Vec::new();
    let mut delim = '\n';
    let mut silent = false;
    let mut i = 0;

    while i < args.len() {
        match args[i].as_str() {
            "-r" => {}  // raw mode (ignore backslash)
            "-s" => { silent = true; }
            "-e" => {}  // readline
            "-u" => {
                i += 1;
                if i < args.len() {
                    fd = args[i].parse().unwrap_or(0);
                }
            }
            "-d" => {
                i += 1;
                if i < args.len() {
                    delim = args[i].chars().next().unwrap_or('\n');
                }
            }
            "-n" | "-N" => { i += 1; } // read N chars - skip flag
            "-p" => { i += 1; } // prompt - skip
            "-a" => {
                i += 1;
                if i < args.len() {
                    var_names.push(args[i].clone());
                }
            }
            s if !s.starts_with('-') => {
                var_names.push(s.to_string());
            }
            _ => {}
        }
        i += 1;
    }

    if var_names.is_empty() {
        var_names.push("REPLY".to_string());
    }

    // Read from fd
    let line = read_line_from_fd(fd, delim);
    let status = match line {
        None => 1,  // EOF
        Some(mut line) => {
            // Strip trailing newline
            if line.ends_with('\n') {
                line.pop();
            }
            if line.ends_with('\r') {
                line.pop();
            }

            let ifs = vars.get_str("IFS").unwrap_or_else(|| " \t\n".to_string());

            if var_names.len() == 1 {
                vars.set(&var_names[0], line);
            } else {
                // Split on IFS
                let parts = split_ifs(&line, &ifs);
                for (idx, name) in var_names.iter().enumerate() {
                    if idx < parts.len() - 1 {
                        vars.set(name, parts[idx].clone());
                    } else if idx == var_names.len() - 1 {
                        // Last var gets rest
                        vars.set(name, parts[idx..].join(&ifs.chars().next().map(|c| c.to_string()).unwrap_or_default()));
                    } else {
                        vars.set(name, String::new());
                    }
                }
            }
            0
        }
    };

    crate::shell::executor::restore_redirections_builtin(saved);
    status
}

fn read_line_from_fd(fd: i32, delim: char) -> Option<String> {
    let delim_byte = delim as u8;
    let mut result = Vec::new();
    let mut buf = [0u8; 1];

    loop {
        // SAFETY: read with valid fd and buf ptr
        let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, 1) };
        if n <= 0 {
            if result.is_empty() {
                return None;
            }
            break;
        }
        if buf[0] == delim_byte {
            if delim == '\n' {
                result.push(buf[0]);
            }
            break;
        }
        result.push(buf[0]);
    }
    Some(String::from_utf8_lossy(&result).into_owned())
}

fn split_ifs(s: &str, ifs: &str) -> Vec<String> {
    if ifs.is_empty() {
        return vec![s.to_string()];
    }
    let ifs_chars: Vec<char> = ifs.chars().collect();
    let mut parts = Vec::new();
    let mut current = String::new();

    for c in s.chars() {
        if ifs_chars.contains(&c) {
            parts.push(current.clone());
            current.clear();
        } else {
            current.push(c);
        }
    }
    parts.push(current);
    parts
}

pub fn builtin_source(args: &[String], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    if args.is_empty() {
        eprintln!("zesh: source: filename required");
        return 1;
    }
    let path = &args[0];
    match std::fs::read_to_string(path) {
        Ok(content) => {
            crate::shell::executor::run_script(&content, path, ctx, vars)
        }
        Err(e) => {
            eprintln!("zesh: {}: {}", path, e);
            1
        }
    }
}

pub fn builtin_test(args: &[String]) -> i32 {
    // Remove trailing ] if present
    let args: Vec<&str> = if args.last().map(|s| s.as_str()) == Some("]") {
        args[..args.len()-1].iter().map(|s| s.as_str()).collect()
    } else {
        args.iter().map(|s| s.as_str()).collect()
    };

    let result = eval_test_expr(&args, &mut 0);
    if result { 0 } else { 1 }
}

fn eval_test_expr(args: &[&str], pos: &mut usize) -> bool {
    if *pos >= args.len() {
        return false;
    }

    // Handle ! negation
    if args[*pos] == "!" {
        *pos += 1;
        return !eval_test_expr(args, pos);
    }

    // Handle ( )
    if args[*pos] == "(" {
        *pos += 1;
        let result = eval_test_or(args, pos);
        if *pos < args.len() && args[*pos] == ")" {
            *pos += 1;
        }
        return result;
    }

    eval_test_or(args, pos)
}

fn eval_test_or(args: &[&str], pos: &mut usize) -> bool {
    let mut left = eval_test_and(args, pos);
    while *pos + 1 < args.len() && args[*pos] == "-o" {
        *pos += 1;
        let right = eval_test_and(args, pos);
        left = left || right;
    }
    left
}

fn eval_test_and(args: &[&str], pos: &mut usize) -> bool {
    let mut left = eval_test_unary(args, pos);
    while *pos + 1 < args.len() && args[*pos] == "-a" {
        *pos += 1;
        let right = eval_test_unary(args, pos);
        left = left && right;
    }
    left
}

fn eval_test_unary(args: &[&str], pos: &mut usize) -> bool {
    if *pos >= args.len() {
        return false;
    }

    // Binary tests
    if *pos + 2 < args.len() {
        let op = args[*pos + 1];
        let a = args[*pos];
        let b = args[*pos + 2];

        match op {
            "=" | "==" => { *pos += 3; return a == b; }
            "!=" => { *pos += 3; return a != b; }
            "-eq" => {
                let na: i64 = a.trim().parse().unwrap_or(0);
                let nb: i64 = b.trim().parse().unwrap_or(0);
                *pos += 3;
                return na == nb;
            }
            "-ne" => {
                let na: i64 = a.trim().parse().unwrap_or(0);
                let nb: i64 = b.trim().parse().unwrap_or(0);
                *pos += 3;
                return na != nb;
            }
            "-lt" => {
                let na: i64 = a.trim().parse().unwrap_or(0);
                let nb: i64 = b.trim().parse().unwrap_or(0);
                *pos += 3;
                return na < nb;
            }
            "-gt" => {
                let na: i64 = a.trim().parse().unwrap_or(0);
                let nb: i64 = b.trim().parse().unwrap_or(0);
                *pos += 3;
                return na > nb;
            }
            "-le" => {
                let na: i64 = a.trim().parse().unwrap_or(0);
                let nb: i64 = b.trim().parse().unwrap_or(0);
                *pos += 3;
                return na <= nb;
            }
            "-ge" => {
                let na: i64 = a.trim().parse().unwrap_or(0);
                let nb: i64 = b.trim().parse().unwrap_or(0);
                *pos += 3;
                return na >= nb;
            }
            "-nt" => {
                let ma = std::fs::metadata(a).and_then(|m| m.modified()).ok();
                let mb = std::fs::metadata(b).and_then(|m| m.modified()).ok();
                *pos += 3;
                return ma > mb;
            }
            "-ot" => {
                let ma = std::fs::metadata(a).and_then(|m| m.modified()).ok();
                let mb = std::fs::metadata(b).and_then(|m| m.modified()).ok();
                *pos += 3;
                return ma < mb;
            }
            "-ef" => {
                // Same inode
                let ia = get_inode(a);
                let ib = get_inode(b);
                *pos += 3;
                return ia.is_some() && ia == ib;
            }
            _ => {}
        }
    }

    // Unary tests
    let arg = args[*pos];

    if arg.starts_with('-') && arg.len() == 2 {
        match &arg[1..2] {
            "z" => {
                let v = args.get(*pos + 1).copied().unwrap_or("");
                *pos += 2;
                return v.is_empty();
            }
            "n" => {
                let v = args.get(*pos + 1).copied().unwrap_or("");
                *pos += 2;
                return !v.is_empty();
            }
            "f" => {
                let path = args.get(*pos + 1).copied().unwrap_or("");
                *pos += 2;
                return std::path::Path::new(path).is_file();
            }
            "d" => {
                let path = args.get(*pos + 1).copied().unwrap_or("");
                *pos += 2;
                return std::path::Path::new(path).is_dir();
            }
            "e" => {
                let path = args.get(*pos + 1).copied().unwrap_or("");
                *pos += 2;
                return std::path::Path::new(path).exists();
            }
            "s" => {
                let path = args.get(*pos + 1).copied().unwrap_or("");
                *pos += 2;
                return std::fs::metadata(path).map(|m| m.len() > 0).unwrap_or(false);
            }
            "r" => {
                let path = args.get(*pos + 1).copied().unwrap_or("");
                *pos += 2;
                let c = std::ffi::CString::new(path).unwrap_or_default();
                return unsafe { libc::access(c.as_ptr(), libc::R_OK) } == 0;
            }
            "w" => {
                let path = args.get(*pos + 1).copied().unwrap_or("");
                *pos += 2;
                let c = std::ffi::CString::new(path).unwrap_or_default();
                return unsafe { libc::access(c.as_ptr(), libc::W_OK) } == 0;
            }
            "x" => {
                let path = args.get(*pos + 1).copied().unwrap_or("");
                *pos += 2;
                let c = std::ffi::CString::new(path).unwrap_or_default();
                return unsafe { libc::access(c.as_ptr(), libc::X_OK) } == 0;
            }
            "L" | "h" => {
                let path = args.get(*pos + 1).copied().unwrap_or("");
                *pos += 2;
                return std::path::Path::new(path).is_symlink();
            }
            "p" => {
                // Named pipe
                *pos += 2;
                return false; // simplified
            }
            "t" => {
                let fd_str = args.get(*pos + 1).copied().unwrap_or("0");
                let fd: i32 = fd_str.parse().unwrap_or(0);
                *pos += 2;
                return unsafe { libc::isatty(fd) } == 1;
            }
            _ => {}
        }
    }

    // Just a string - true if non-empty
    *pos += 1;
    !arg.is_empty()
}

fn get_inode(path: &str) -> Option<u64> {
    use std::os::unix::fs::MetadataExt;
    std::fs::metadata(path).ok().map(|m| m.ino())
}

pub fn builtin_eval(args: &[String], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    let code = args.join(" ");
    crate::shell::executor::run_script(&code, &ctx.script_file.clone(), ctx, vars)
}

pub fn builtin_type(args: &[String], vars: &VarStore) -> i32 {
    let mut status = 0;
    for arg in args {
        // Check functions
        if vars.functions.contains_key(arg.as_str()) {
            println!("{} is a function", arg);
            continue;
        }
        // Check builtins
        if is_builtin(arg) {
            println!("{} is a shell builtin", arg);
            continue;
        }
        // Check keywords
        if is_keyword(arg) {
            println!("{} is a shell keyword", arg);
            continue;
        }
        // Check PATH
        if let Some(path) = crate::shell::executor::find_in_path(arg, vars) {
            println!("{} is {}", arg, path);
            continue;
        }
        eprintln!("zesh: type: {}: not found", arg);
        status = 1;
    }
    status
}

pub fn is_builtin(name: &str) -> bool {
    matches!(name,
        "echo" | "printf" | "cd" | "pwd" | "exit" | "return" | "break" | "continue" |
        "set" | "unset" | "export" | "readonly" | "local" | "declare" | "typeset" |
        "read" | "source" | "." | "true" | "false" | "test" | "[" | "eval" |
        "type" | "hash" | "wait" | "jobs" | "kill" | "trap" | "umask" | "ulimit" |
        "getopts" | "mapfile" | "readarray" | "caller" | "compgen" | "complete" |
        "disown" | "shift" | "let" | "builtin" | "command" | "times" | "suspend" |
        "fc" | "history" | "dirs" | "popd" | "pushd" | "exec"
    )
}

pub fn is_keyword(name: &str) -> bool {
    matches!(name,
        "if" | "then" | "else" | "elif" | "fi" |
        "while" | "until" | "do" | "done" |
        "for" | "in" | "case" | "esac" |
        "select" | "function" | "time" | "coproc" |
        "{" | "}" | "!" | "[[" | "]]"
    )
}

pub fn builtin_hash(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);

    let status = if args.is_empty() {
        // Print all cached entries
        let table = vars.hash_table.clone();
        if table.is_empty() {
            // Nothing to print
        } else {
            for (k, v) in &table {
                println!("hits\tcommand\n\t{}", v);
            }
        }
        0
    } else {
        let mut i = 0;
        let mut s = 0;
        while i < args.len() {
            match args[i].as_str() {
                "-r" => {
                    vars.hash_table.clear();
                }
                "-d" => {
                    i += 1;
                    if i < args.len() {
                        vars.hash_table.remove(&args[i]);
                    }
                }
                "-l" => {
                    // List
                    for (k, v) in &vars.hash_table.clone() {
                        println!("builtin hash -p {} {}", v, k);
                    }
                }
                "-p" => {
                    i += 1;
                    if i + 1 < args.len() {
                        let path = args[i].clone();
                        i += 1;
                        let name = args[i].clone();
                        vars.hash_table.insert(name, path);
                    }
                }
                name => {
                    // Add to cache
                    if let Some(path) = crate::shell::executor::find_in_path(name, vars) {
                        vars.hash_table.insert(name.to_string(), path);
                    } else {
                        eprintln!("zesh: hash: {}: not found", name);
                        s = 1;
                    }
                }
            }
            i += 1;
        }
        s
    };

    crate::shell::executor::restore_redirections_builtin(saved);
    status
}

pub fn builtin_wait(args: &[String], vars: &mut VarStore) -> i32 {
    if args.is_empty() {
        // Wait for all background jobs
        let pids = crate::shell::jobs::jobs().all_pids();
        let mut last_status = 0;
        for pid in pids {
            let mut wstatus = 0;
            // SAFETY: waitpid with valid pid
            if unsafe { libc::waitpid(pid, &mut wstatus, 0) } > 0 {
                if libc::WIFEXITED(wstatus) {
                    last_status = libc::WEXITSTATUS(wstatus);
                }
            }
            crate::shell::jobs::jobs().remove(pid);
        }
        return last_status;
    }

    let mut status = 0;
    for arg in args {
        let pid: i32 = match arg.parse() {
            Ok(p) => p,
            Err(_) => {
                eprintln!("zesh: wait: {}: invalid PID", arg);
                status = 1;
                continue;
            }
        };

        let mut wstatus = 0;
        // SAFETY: waitpid with valid pid
        let result = unsafe { libc::waitpid(pid, &mut wstatus, 0) };
        if result < 0 {
            eprintln!("zesh: wait: {}: no such process", pid);
            status = 127;
        } else {
            if libc::WIFEXITED(wstatus) {
                status = libc::WEXITSTATUS(wstatus);
            } else if libc::WIFSIGNALED(wstatus) {
                status = 128 + libc::WTERMSIG(wstatus);
            }
            crate::shell::jobs::jobs().remove(pid);
        }
    }
    status
}

pub fn builtin_jobs(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);

    let jobs = crate::shell::jobs::jobs();
    let mut job_ids: Vec<usize> = jobs.jobs.keys().copied().collect();
    job_ids.sort();
    for id in job_ids {
        if let Some(job) = jobs.jobs.get(&id) {
            println!("[{}] {} {}", id, job.pid, job.cmd);
        }
    }

    crate::shell::executor::restore_redirections_builtin(saved);
    0
}

pub fn builtin_kill(args: &[String]) -> i32 {
    let mut sig = libc::SIGTERM;
    let mut start = 0;

    if let Some(first) = args.first() {
        if first.starts_with('-') {
            let sig_str = &first[1..];
            sig = match sig_str {
                "0" => 0,
                "1" | "HUP" => libc::SIGHUP,
                "2" | "INT" => libc::SIGINT,
                "9" | "KILL" => libc::SIGKILL,
                "15" | "TERM" => libc::SIGTERM,
                s => s.parse().unwrap_or(libc::SIGTERM),
            };
            start = 1;
        }
    }

    for arg in &args[start..] {
        if let Ok(pid) = arg.parse::<i32>() {
            // SAFETY: kill with valid pid and signal
            unsafe { libc::kill(pid, sig); }
        }
    }
    0
}

pub fn builtin_trap(args: &[String], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    if args.is_empty() {
        // List traps
        return 0;
    }

    if args[0] == "-p" || args[0] == "-l" {
        return 0;
    }

    // trap ACTION SIGNAL...
    let action = &args[0];
    let signals = &args[1..];

    if signals.is_empty() {
        return 0;
    }

    for sig in signals {
        match sig.as_str() {
            "EXIT" | "0" => {
                if let Ok(mut trap) = crate::shell::signals::G_TRAP_EXIT.lock() {
                    if action == "-" || action.is_empty() {
                        *trap = None;
                    } else {
                        *trap = Some(action.clone());
                    }
                }
            }
            _ => {}
        }
    }
    0
}

pub fn builtin_umask(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);

    let status = if args.is_empty() {
        // Get current umask
        // SAFETY: umask(0) reads current mask, then restore it
        let mask = unsafe { libc::umask(0) };
        // SAFETY: restore the mask
        unsafe { libc::umask(mask); }
        println!("{:04o}", mask);
        0
    } else {
        // Set umask
        let mask_str = &args[0];
        // Remove optional leading 0 padding
        match u32::from_str_radix(mask_str.trim_start_matches("0x"), 8) {
            Ok(mask) => {
                // SAFETY: umask is always safe to call
                unsafe { libc::umask(mask as libc::mode_t); }
                0
            }
            Err(_) => {
                // Try decimal
                match mask_str.parse::<u32>() {
                    Ok(mask) => {
                        // SAFETY: umask is always safe to call
                        unsafe { libc::umask(mask as libc::mode_t); }
                        0
                    }
                    Err(_) => {
                        eprintln!("zesh: umask: {}: invalid mask", mask_str);
                        1
                    }
                }
            }
        }
    };

    crate::shell::executor::restore_redirections_builtin(saved);
    status
}

pub fn builtin_ulimit(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);

    let status = if args.is_empty() || (args.len() == 1 && args[0] == "-a") {
        // Print all limits
        print_all_ulimits();
        0
    } else {
        let mut i = 0;
        let mut resource_flag = 'n'; // default: nofile
        let mut set_value: Option<u64> = None;
        let mut get_only = true;

        while i < args.len() {
            let arg = &args[i];
            if arg.starts_with('-') {
                for c in arg[1..].chars() {
                    match c {
                        'a' => {
                            print_all_ulimits();
                            crate::shell::executor::restore_redirections_builtin(saved);
                            return 0;
                        }
                        'n' | 'v' | 'u' | 's' | 'c' | 'd' | 'f' | 'l' | 'm' | 'e' | 'r' | 'i' | 'q' | 'x' | 'p' | 'k' | 't' => {
                            resource_flag = c;
                        }
                        _ => {}
                    }
                }
            } else {
                // Value to set
                if let Ok(v) = arg.parse::<u64>() {
                    set_value = Some(v);
                    get_only = false;
                } else if arg == "unlimited" {
                    set_value = Some(u64::MAX);
                    get_only = false;
                }
            }
            i += 1;
        }

        let resource = match resource_flag {
            'n' => libc::RLIMIT_NOFILE,
            'v' => libc::RLIMIT_AS,
            's' => libc::RLIMIT_STACK,
            'c' => libc::RLIMIT_CORE,
            'd' => libc::RLIMIT_DATA,
            'f' => libc::RLIMIT_FSIZE,
            'm' => libc::RLIMIT_RSS,
            't' => libc::RLIMIT_CPU,
            'u' => libc::RLIMIT_NPROC,
            _ => libc::RLIMIT_NOFILE,
        };

        if get_only {
            let mut rl: libc::rlimit = unsafe { std::mem::zeroed() };
            // SAFETY: getrlimit with valid resource and ptr
            if unsafe { libc::getrlimit(resource, &mut rl) } == 0 {
                if rl.rlim_cur == libc::RLIM_INFINITY {
                    println!("unlimited");
                } else {
                    println!("{}", rl.rlim_cur);
                }
                0
            } else {
                1
            }
        } else if let Some(val) = set_value {
            let mut rl: libc::rlimit = unsafe { std::mem::zeroed() };
            // SAFETY: getrlimit with valid ptr
            unsafe { libc::getrlimit(resource, &mut rl); }
            let new_val = if val == u64::MAX { libc::RLIM_INFINITY } else { val };
            rl.rlim_cur = new_val;
            // SAFETY: setrlimit with valid resource and ptr
            if unsafe { libc::setrlimit(resource, &rl) } == 0 {
                0
            } else {
                eprintln!("zesh: ulimit: {}", std::io::Error::last_os_error());
                1
            }
        } else {
            0
        }
    };

    crate::shell::executor::restore_redirections_builtin(saved);
    status
}

fn print_all_ulimits() {
    let limits: &[(&str, libc::__rlimit_resource_t, &str)] = &[
        ("core file size          (blocks, -c)", libc::RLIMIT_CORE, "-c"),
        ("data seg size           (kbytes, -d)", libc::RLIMIT_DATA, "-d"),
        ("scheduling priority             (-e)", libc::RLIMIT_NICE, "-e"),
        ("file size               (blocks, -f)", libc::RLIMIT_FSIZE, "-f"),
        ("pending signals                 (-i)", libc::RLIMIT_SIGPENDING, "-i"),
        ("max locked memory       (kbytes, -l)", libc::RLIMIT_MEMLOCK, "-l"),
        ("max memory size         (kbytes, -m)", libc::RLIMIT_RSS, "-m"),
        ("open files                      (-n)", libc::RLIMIT_NOFILE, "-n"),
        ("pipe size            (512 bytes, -p)", libc::RLIMIT_MSGQUEUE, "-p"),  // using MSGQUEUE as placeholder
        ("POSIX message queues     (bytes, -q)", libc::RLIMIT_MSGQUEUE, "-q"),
        ("real-time priority              (-r)", libc::RLIMIT_RTPRIO, "-r"),
        ("stack size              (kbytes, -s)", libc::RLIMIT_STACK, "-s"),
        ("cpu time               (seconds, -t)", libc::RLIMIT_CPU, "-t"),
        ("max user processes              (-u)", libc::RLIMIT_NPROC, "-u"),
        ("virtual memory          (kbytes, -v)", libc::RLIMIT_AS, "-v"),
        ("file locks                      (-x)", libc::RLIMIT_LOCKS, "-x"),
    ];

    for (desc, resource, _flag) in limits {
        let mut rl: libc::rlimit = unsafe { std::mem::zeroed() };
        // SAFETY: getrlimit with valid ptr
        if unsafe { libc::getrlimit(*resource, &mut rl) } == 0 {
            if rl.rlim_cur == libc::RLIM_INFINITY {
                println!("{:40} unlimited", desc);
            } else {
                println!("{:40} {}", desc, rl.rlim_cur);
            }
        }
    }
}

pub fn builtin_getopts(args: &[String], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    if args.len() < 2 {
        return 1;
    }

    let optstring = &args[0];
    let varname = &args[1];

    // Get OPTIND
    let optind: usize = vars.get_str("OPTIND").unwrap_or_else(|| "1".to_string())
        .trim().parse().unwrap_or(1);

    let pos_params = if ctx.pos_params.is_empty() {
        // Get from vars
        let mut pp = Vec::new();
        let mut i = 1;
        loop {
            if let Some(v) = vars.get_str(&i.to_string()) {
                pp.push(v);
                i += 1;
            } else {
                break;
            }
        }
        pp
    } else {
        ctx.pos_params.clone()
    };

    // Adjust for 1-based OPTIND
    if optind > pos_params.len() {
        return 1; // Done
    }

    let current_arg = &pos_params[optind - 1];

    if !current_arg.starts_with('-') || current_arg == "-" {
        return 1; // No more options
    }

    if current_arg == "--" {
        vars.set_raw("OPTIND", (optind + 1).to_string(), 0);
        return 1;
    }

    // Get position within current arg (OPTIND is 1-based, optind_inner tracks within arg)
    // Use OPTIND to track both arg index and position within arg
    // We use a simple approach: scan arg chars starting from position 1 (past -)
    let chars: Vec<char> = current_arg.chars().collect();
    // Find which character in the current arg we're processing
    // We track this via a secondary var, but simplified: process one char per call
    // The standard way: OPTIND points to the positional arg, and we use another internal var
    // Simplified: process chars[1..] one at a time, advancing OPTIND when done with arg

    // Let's track position within arg via a simple approach:
    // We use the upper bits of a u64 stored in a var
    let inner_pos: usize = vars.get_str("_GETOPTS_POS").unwrap_or_else(|| "1".to_string())
        .parse().unwrap_or(1);

    let opt_pos = if inner_pos == 0 { 1 } else { inner_pos };

    if opt_pos >= chars.len() {
        // Move to next arg
        vars.set_raw("OPTIND", (optind + 1).to_string(), 0);
        vars.set_raw("_GETOPTS_POS", "1".to_string(), 0);
        return builtin_getopts(args, ctx, vars);
    }

    let opt = chars[opt_pos];
    let next_pos = opt_pos + 1;

    // Find opt in optstring
    let optstring_chars: Vec<char> = optstring.chars().collect();
    let silent = optstring_chars.first() == Some(&':');
    let search_start = if silent { 1 } else { 0 };

    let mut found = false;
    let mut needs_arg = false;
    let mut search_i = search_start;
    while search_i < optstring_chars.len() {
        if optstring_chars[search_i] == opt {
            found = true;
            needs_arg = optstring_chars.get(search_i + 1) == Some(&':');
            break;
        }
        search_i += 1;
    }

    if !found {
        vars.set_raw(varname, "?".to_string(), 0);
        vars.set_raw("OPTARG", String::new(), 0);
        // Advance
        if next_pos >= chars.len() {
            vars.set_raw("OPTIND", (optind + 1).to_string(), 0);
            vars.set_raw("_GETOPTS_POS", "1".to_string(), 0);
        } else {
            vars.set_raw("_GETOPTS_POS", next_pos.to_string(), 0);
        }
        return 0;
    }

    vars.set_raw(varname, opt.to_string(), 0);

    if needs_arg {
        // Check if arg is attached or in next positional
        if next_pos < chars.len() {
            // Attached: -bval
            let optarg: String = chars[next_pos..].iter().collect();
            vars.set_raw("OPTARG", optarg, 0);
            vars.set_raw("OPTIND", (optind + 1).to_string(), 0);
            vars.set_raw("_GETOPTS_POS", "1".to_string(), 0);
        } else {
            // Next positional
            if optind < pos_params.len() {
                let optarg = pos_params[optind].clone();
                vars.set_raw("OPTARG", optarg, 0);
                vars.set_raw("OPTIND", (optind + 2).to_string(), 0);
                vars.set_raw("_GETOPTS_POS", "1".to_string(), 0);
            } else {
                // Missing argument
                if silent {
                    vars.set_raw(varname, ":".to_string(), 0);
                } else {
                    eprintln!("zesh: getopts: option -{} requires an argument", opt);
                    vars.set_raw(varname, "?".to_string(), 0);
                }
                vars.set_raw("OPTARG", String::new(), 0);
                vars.set_raw("OPTIND", (optind + 1).to_string(), 0);
                vars.set_raw("_GETOPTS_POS", "1".to_string(), 0);
            }
        }
    } else {
        vars.set_raw("OPTARG", String::new(), 0);
        if next_pos >= chars.len() {
            vars.set_raw("OPTIND", (optind + 1).to_string(), 0);
            vars.set_raw("_GETOPTS_POS", "1".to_string(), 0);
        } else {
            vars.set_raw("_GETOPTS_POS", next_pos.to_string(), 0);
        }
    }

    0
}

pub fn builtin_mapfile(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);

    let mut strip_newline = false;
    let mut arr_name = "MAPFILE".to_string();
    let mut start_idx = 0usize;
    let mut count: Option<usize> = None;
    let mut fd = 0i32;

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "-t" => { strip_newline = true; }
            "-u" => {
                i += 1;
                if i < args.len() {
                    fd = args[i].parse().unwrap_or(0);
                }
            }
            "-O" => {
                i += 1;
                if i < args.len() {
                    start_idx = args[i].parse().unwrap_or(0);
                }
            }
            "-n" => {
                i += 1;
                if i < args.len() {
                    count = args[i].parse().ok();
                }
            }
            "-d" => { i += 1; } // delimiter - skip
            "-c" | "-C" => { i += 1; } // callback - skip
            s if !s.starts_with('-') => {
                arr_name = s.to_string();
            }
            _ => {}
        }
        i += 1;
    }

    // Read lines from stdin (fd 0) or specified fd
    let mut lines = Vec::new();
    let mut buf = [0u8; 1];
    let mut line = Vec::new();
    let max_lines = count.unwrap_or(usize::MAX);

    loop {
        if lines.len() >= max_lines {
            break;
        }
        // SAFETY: read with valid fd and buf ptr
        let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, 1) };
        if n <= 0 {
            if !line.is_empty() {
                lines.push(String::from_utf8_lossy(&line).into_owned());
            }
            break;
        }
        if buf[0] == b'\n' {
            let mut s = String::from_utf8_lossy(&line).into_owned();
            if !strip_newline {
                s.push('\n');
            }
            lines.push(s);
            line.clear();
        } else {
            line.push(buf[0]);
        }
    }

    // Clear the array first
    vars.arrays.remove(&arr_name);

    // Fill array
    for (i, s) in lines.iter().enumerate() {
        vars.set_array_elem(&arr_name, start_idx + i, s.clone());
    }

    crate::shell::executor::restore_redirections_builtin(saved);
    0
}

pub fn builtin_caller(args: &[String], ctx: &ExecContext) -> i32 {
    let frame: usize = args.first()
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);

    let stack_len = ctx.funcname.len();
    if stack_len == 0 {
        return 1;
    }

    // frame 0 = most recent call
    let idx = if frame < stack_len { stack_len - 1 - frame } else { return 1; };

    let lineno = ctx.funcname_lineno.get(idx).copied().unwrap_or(ctx.lineno);
    let fname = ctx.funcname.get(idx).map(|s| s.as_str()).unwrap_or("main");
    let source = &ctx.script_file;

    println!("{} {} {}", lineno, fname, source);
    0
}

pub fn builtin_compgen(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);

    let mut i = 0;
    let mut status = 0;

    while i < args.len() {
        match args[i].as_str() {
            "-b" => {
                // Print builtins
                let builtins = get_all_builtins();
                for b in builtins {
                    println!("{}", b);
                }
            }
            "-k" => {
                // Print keywords
                let keywords = get_all_keywords();
                for k in keywords {
                    println!("{}", k);
                }
            }
            "-c" => {
                // Commands with prefix
                i += 1;
                let prefix = args.get(i).map(|s| s.as_str()).unwrap_or("");
                // Search PATH
                let path = vars.get_str("PATH").unwrap_or_else(|| "/usr/local/bin:/usr/bin:/bin".to_string());
                let mut found: Vec<String> = Vec::new();
                for dir in path.split(':') {
                    if let Ok(entries) = std::fs::read_dir(dir) {
                        for entry in entries.flatten() {
                            let name = entry.file_name().to_string_lossy().into_owned();
                            if name.starts_with(prefix) {
                                found.push(name);
                            }
                        }
                    }
                }
                found.sort();
                found.dedup();
                for f in found {
                    println!("{}", f);
                }
                // Also include builtins and keywords
                for b in get_all_builtins() {
                    if b.starts_with(prefix) {
                        println!("{}", b);
                    }
                }
            }
            "-v" => {
                // Variables with prefix
                i += 1;
                let prefix = args.get(i).map(|s| s.as_str()).unwrap_or("");
                let all = vars.all_vars();
                let mut names: Vec<String> = all.keys()
                    .filter(|k| k.starts_with(prefix))
                    .cloned()
                    .collect();
                names.sort();
                for n in names {
                    println!("{}", n);
                }
            }
            "-f" => {
                // Files - skip
            }
            "-d" => {
                // Dirs - skip
            }
            "-u" => {
                // Users - skip
            }
            "-g" => {
                // Groups - skip
            }
            "-a" => {
                // Aliases - skip
            }
            "-A" => {
                i += 1; // skip action type
            }
            _ => {}
        }
        i += 1;
    }

    crate::shell::executor::restore_redirections_builtin(saved);
    status
}

fn get_all_builtins() -> Vec<&'static str> {
    vec![
        "echo", "printf", "cd", "pwd", "exit", "return", "break", "continue",
        "set", "unset", "export", "readonly", "local", "declare", "typeset",
        "read", "source", ".", "true", "false", "test", "[", "eval",
        "type", "hash", "wait", "jobs", "kill", "trap", "umask", "ulimit",
        "getopts", "mapfile", "readarray", "caller", "compgen", "complete",
        "disown", "shift", "let", "builtin", "command", "times", "suspend",
        "fc", "history", "dirs", "popd", "pushd", "exec", ":",
    ]
}

fn get_all_keywords() -> Vec<&'static str> {
    vec![
        "if", "then", "else", "elif", "fi",
        "while", "until", "do", "done",
        "for", "in", "case", "esac",
        "select", "function", "time", "coproc",
        "{", "}", "!", "[[", "]]",
    ]
}

pub fn builtin_disown(args: &[String], vars: &mut VarStore) -> i32 {
    if args.is_empty() {
        // Disown last job
        let last_pid: i32 = vars.get_str("!").and_then(|s| s.parse().ok()).unwrap_or(0);
        if last_pid > 0 {
            crate::shell::jobs::jobs().disown(last_pid);
        }
        return 0;
    }

    for arg in args {
        if arg.starts_with('%') {
            // Job number
            continue;
        }
        if let Ok(pid) = arg.parse::<i32>() {
            crate::shell::jobs::jobs().disown(pid);
        }
    }
    0
}

pub fn builtin_shift(args: &[String], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    let n = args.first().and_then(|s| s.parse::<usize>().ok()).unwrap_or(1);

    // Shift positional params
    let count = ctx.pos_params.len();
    if n > count {
        return 1;
    }
    ctx.pos_params = ctx.pos_params[n..].to_vec();

    // Update $1, $2, ... in vars
    for (i, p) in ctx.pos_params.iter().enumerate() {
        vars.set_raw(&(i+1).to_string(), p.clone(), 0);
    }
    // Unset vars beyond new length
    for i in ctx.pos_params.len()..count {
        vars.unset(&(i+1).to_string());
    }
    vars.set_raw("#", ctx.pos_params.len().to_string(), 0);
    0
}

pub fn builtin_let(args: &[String], vars: &mut VarStore) -> i32 {
    let mut last = 0i64;
    for expr in args {
        last = match eval_arith_simple(expr) {
            Ok(n) => n,
            Err(_) => 0,
        };
    }
    if last == 0 { 1 } else { 0 }
}

pub fn builtin_history(args: &[String], redirs: &[FdRedir], ctx: &mut ExecContext, vars: &mut VarStore) -> i32 {
    use crate::shell::executor::apply_redirections_for_builtin;
    let saved = apply_redirections_for_builtin(redirs, ctx, vars);
    // Simple stub
    crate::shell::executor::restore_redirections_builtin(saved);
    0
}
