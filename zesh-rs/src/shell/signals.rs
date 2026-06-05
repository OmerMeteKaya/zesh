// Signal handling

use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::Mutex;

pub static G_SIGINT_RECEIVED: AtomicBool = AtomicBool::new(false);
pub static G_INTERRUPT_LOOP: AtomicBool = AtomicBool::new(false);
pub static G_FOREGROUND_PID: AtomicI32 = AtomicI32::new(-1);

// Trap actions: index = signal number
pub static G_TRAP_ACTIONS: Mutex<[Option<String>; 32]> = Mutex::new([
    None, None, None, None, None, None, None, None,
    None, None, None, None, None, None, None, None,
    None, None, None, None, None, None, None, None,
    None, None, None, None, None, None, None, None,
]);
pub static G_TRAP_EXIT: Mutex<Option<String>> = Mutex::new(None);

extern "C" fn handle_sigint(_sig: libc::c_int) {
    G_SIGINT_RECEIVED.store(true, Ordering::SeqCst);
    G_INTERRUPT_LOOP.store(true, Ordering::SeqCst);

    let fgpid = G_FOREGROUND_PID.load(Ordering::SeqCst);
    if fgpid > 0 {
        // SAFETY: sending SIGINT to a process group is valid
        unsafe { libc::kill(fgpid, libc::SIGINT); }
    }
}

extern "C" fn handle_sigchld(_sig: libc::c_int) {
    // Reap any zombie children
    loop {
        let pid = unsafe { libc::waitpid(-1, std::ptr::null_mut(), libc::WNOHANG) };
        if pid <= 0 { break; }
    }
}

pub fn setup_signals() {
    // SAFETY: setting up signal handlers with valid function pointers
    unsafe {
        let mut sa: libc::sigaction = std::mem::zeroed();
        sa.sa_sigaction = handle_sigint as usize;
        libc::sigemptyset(&mut sa.sa_mask);
        sa.sa_flags = libc::SA_RESTART;
        libc::sigaction(libc::SIGINT, &sa, std::ptr::null_mut());

        // SIGCHLD
        let mut sa2: libc::sigaction = std::mem::zeroed();
        sa2.sa_sigaction = handle_sigchld as usize;
        libc::sigemptyset(&mut sa2.sa_mask);
        sa2.sa_flags = libc::SA_RESTART | libc::SA_NOCLDSTOP;
        libc::sigaction(libc::SIGCHLD, &sa2, std::ptr::null_mut());

        // Ignore SIGPIPE
        let mut sa3: libc::sigaction = std::mem::zeroed();
        sa3.sa_sigaction = libc::SIG_IGN;
        libc::sigemptyset(&mut sa3.sa_mask);
        libc::sigaction(libc::SIGPIPE, &sa3, std::ptr::null_mut());

        // Ignore SIGTTOU/SIGTTIN in interactive mode
        let mut sa4: libc::sigaction = std::mem::zeroed();
        sa4.sa_sigaction = libc::SIG_IGN;
        libc::sigemptyset(&mut sa4.sa_mask);
        libc::sigaction(libc::SIGTTOU, &sa4, std::ptr::null_mut());
        libc::sigaction(libc::SIGTTIN, &sa4, std::ptr::null_mut());
    }
}

pub fn reset_signals_for_child() {
    // SAFETY: resetting signals to default in child process
    unsafe {
        let mut sa: libc::sigaction = std::mem::zeroed();
        sa.sa_sigaction = libc::SIG_DFL;
        libc::sigemptyset(&mut sa.sa_mask);
        libc::sigaction(libc::SIGINT, &sa, std::ptr::null_mut());
        libc::sigaction(libc::SIGPIPE, &sa, std::ptr::null_mut());
        libc::sigaction(libc::SIGTTOU, &sa, std::ptr::null_mut());
        libc::sigaction(libc::SIGTTIN, &sa, std::ptr::null_mut());
    }
}

pub fn run_exit_trap(action: &str, vars: &crate::shell::vars::VarStore, script_file: &str) {
    let tokens = crate::shell::lexer::lex(action);
    let nodes = crate::shell::parser::parse(tokens);
    let mut ctx = crate::shell::executor::ExecContext::new_subshell();
    ctx.script_file = script_file.to_string();
    crate::shell::executor::execute_list_with_vars(&nodes, &mut ctx, vars);
}
