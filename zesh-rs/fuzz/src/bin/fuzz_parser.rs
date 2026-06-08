use afl::fuzz;
use zesh_rs::shell::lexer::lex;
use zesh_rs::shell::parser::parse;

fn main() {
    let tmp = std::env::temp_dir().join("zesh_fuzz_empty");
    std::fs::create_dir_all(&tmp).ok();
    std::env::set_current_dir(&tmp).ok();
    std::env::set_var("PATH", "");
    fuzz!(|data: &[u8]| {
        if let Ok(input) = std::str::from_utf8(data) {
            let tokens = lex(input);
            let _nodes = parse(tokens);
        }
    });
}
