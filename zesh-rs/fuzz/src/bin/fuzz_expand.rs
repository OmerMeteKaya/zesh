use afl::fuzz;
use zesh_rs::shell::vars::{reset_fuzz_state, VarStore};
use zesh_rs::shell::expand::expand_word;

fn main() {
    let tmp = std::env::temp_dir().join("zesh_fuzz_empty");
    std::fs::create_dir_all(&tmp).ok();
    std::env::set_current_dir(&tmp).ok();
    std::env::set_var("PATH", "");
    fuzz!(|data: &[u8]| {
        if let Ok(input) = std::str::from_utf8(data) {
            reset_fuzz_state();
            let vars = VarStore::new();
            let _result = expand_word(input, false, &vars, "");
        }
    });
}
