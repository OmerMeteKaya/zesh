// Zesh standalone binary entry point

mod shell;

use shell::executor::{ExecContext, run_script};
use shell::vars::VarStore;

fn main() {
    let args: Vec<String> = std::env::args().collect();

    // Initialize subsystems
    let _config = shell::config::config_load();
    shell::signals::setup_signals();
    shell::history::history_init();

    // Create VarStore (not using global Mutex during execution)
    let mut vars = VarStore::new();
    let mut ctx = ExecContext::new();

    // Initialize environment
    vars.set_raw("$", std::process::id().to_string(), 0);
    vars.set_raw("0", args[0].clone(), 0);
    vars.set_raw("BASH_SOURCE", args[0].clone(), 0);
    vars.set_raw("BASH_VERSION", "5.2.0(1)-release".to_string(), 0);
    vars.set_raw("SECONDS", "0".to_string(), 0);
    let home = std::env::var("HOME").unwrap_or_else(|_| "/".to_string());
    vars.set_raw("HOME", home, 0);
    let pwd = std::env::current_dir()
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_else(|_| "/".to_string());
    vars.set_raw("PWD", pwd.clone(), 0);
    ctx.cwd = std::path::PathBuf::from(&pwd);

    // Export all env vars
    for (k, val) in std::env::vars() {
        vars.set_raw(&k, val, crate::shell::vars::ATTR_EXPORT);
    }

    // Ensure $PATH is set
    if vars.get_str("PATH").is_none() {
        vars.set_raw("PATH", "/usr/local/bin:/usr/bin:/bin".to_string(), crate::shell::vars::ATTR_EXPORT);
    }

    // Seed random
    unsafe { libc::srand(libc::time(std::ptr::null_mut()) as u32); }

    // Record start time for SECONDS
    let start_time = std::time::Instant::now();

    if args.len() >= 3 && args[1] == "-c" {
        let cmd = &args[2];
        for (i, arg) in args[3..].iter().enumerate() {
            vars.set_raw(&(i+1).to_string(), arg.clone(), 0);
        }
        vars.set_raw("#", args[3..].len().to_string(), 0);
        ctx.pos_params = args[3..].to_vec();
        let status = run_script(cmd, "-c", &mut ctx, &mut vars);
        std::process::exit(status);
    } else if args.len() >= 2 && !args[1].starts_with('-') {
        let script_file = args[1].clone();
        vars.set_raw("0", script_file.clone(), 0);
        vars.set_raw("BASH_SOURCE", script_file.clone(), 0);
        for (i, arg) in args[2..].iter().enumerate() {
            vars.set_raw(&(i+1).to_string(), arg.clone(), 0);
        }
        vars.set_raw("#", args[2..].len().to_string(), 0);
        ctx.script_file = script_file.clone();
        ctx.pos_params = args[2..].to_vec();

        let content = match std::fs::read_to_string(&script_file) {
            Ok(c) => c,
            Err(e) => {
                eprintln!("zesh: {}: {}", script_file, e);
                std::process::exit(1);
            }
        };

        let status = run_script(&content, &script_file, &mut ctx, &mut vars);

        // Run EXIT trap
        {
            let trap_action = if let Ok(trap) = crate::shell::signals::G_TRAP_EXIT.lock() {
                trap.clone()
            } else {
                None
            };
            if let Some(action) = trap_action {
                crate::shell::signals::run_exit_trap(&action, &vars, &ctx.script_file.clone());
            }
        }

        std::process::exit(status);
    } else {
        // Interactive REPL
        run_interactive(&mut ctx, &mut vars, start_time);
    }
}

fn run_interactive(ctx: &mut ExecContext, vars: &mut VarStore, start_time: std::time::Instant) {
    use shell::input::read_line;

    loop {
        // Update SECONDS
        let secs = start_time.elapsed().as_secs();
        vars.set_raw("SECONDS", secs.to_string(), 0);

        let ps1 = vars.get_str("PS1").unwrap_or_else(|| "zesh$ ".to_string());

        let line = match read_line(&ps1) {
            Some(l) => l,
            None => break,
        };

        if line.trim().is_empty() {
            continue;
        }

        shell::history::history_add(line.clone());

        let mut full_line = line;
        while shell::input::is_incomplete(&full_line) {
            let ps2 = vars.get_str("PS2").unwrap_or_else(|| "> ".to_string());
            match read_line(&ps2) {
                Some(next) => {
                    full_line.push('\n');
                    full_line.push_str(&next);
                }
                None => break,
            }
        }

        let status = run_script(&full_line, "stdin", ctx, vars);
        vars.set_raw("?", status.to_string(), 0);
    }
}
