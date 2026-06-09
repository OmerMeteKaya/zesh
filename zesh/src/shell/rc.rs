// .zeshrc loading

pub fn load_rc(ctx: &mut crate::shell::executor::ExecContext, vars: &mut crate::shell::vars::VarStore) {
    // Try to load ~/.zeshrc
    if let Ok(home) = std::env::var("HOME") {
        let rc_path = format!("{}/.zeshrc", home);
        if std::path::Path::new(&rc_path).exists() {
            if let Ok(content) = std::fs::read_to_string(&rc_path) {
                crate::shell::executor::run_script(&content, &rc_path, ctx, vars);
            }
        }
    }
}
