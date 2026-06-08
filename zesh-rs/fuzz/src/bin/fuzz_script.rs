use afl::fuzz;
use zesh_rs::shell::lexer::lex;
use zesh_rs::shell::parser::parse;
use zesh_rs::shell::executor::{execute_list, ExecContext};
use zesh_rs::shell::vars::{reset_fuzz_state, VarStore};

fn main() {
    std::env::set_var("PATH", "");
    let tmp = std::env::temp_dir().join("zesh_fuzz_empty");
    std::fs::create_dir_all(&tmp).ok();
    std::env::set_current_dir(&tmp).ok();

    fuzz!(|data: &[u8]| {
        if let Ok(input) = std::str::from_utf8(data) {
            reset_fuzz_state();

            let input_owned = input.to_owned();
            let handle = std::thread::spawn(move || {
                let tokens = lex(&input_owned);
                let nodes = parse(tokens);
                let mut ctx = ExecContext::new();
                let mut vars = VarStore::new();
                let _exit = execute_list(&nodes, &mut ctx, &mut vars);
            });
            let _ = handle.join();
        }
    });
}
