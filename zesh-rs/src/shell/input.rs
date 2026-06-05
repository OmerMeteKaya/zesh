// Basic readline input

use std::io::{self, Write, BufRead};

pub fn read_line(prompt: &str) -> Option<String> {
    print!("{}", prompt);
    let _ = io::stdout().flush();

    let stdin = io::stdin();
    let mut line = String::new();
    match stdin.lock().read_line(&mut line) {
        Ok(0) => None,  // EOF
        Ok(_) => {
            // Remove trailing newline
            if line.ends_with('\n') {
                line.pop();
                if line.ends_with('\r') {
                    line.pop();
                }
            }
            Some(line)
        }
        Err(_) => None,
    }
}

pub fn is_incomplete(line: &str) -> bool {
    // Check if the input looks incomplete (unmatched quotes, etc.)
    let mut in_single = false;
    let mut in_double = false;
    let mut paren_depth = 0i32;
    let mut brace_depth = 0i32;

    let chars: Vec<char> = line.chars().collect();
    let mut i = 0;

    while i < chars.len() {
        match chars[i] {
            '\'' if !in_double => { in_single = !in_single; }
            '"' if !in_single => { in_double = !in_double; }
            '(' if !in_single && !in_double => { paren_depth += 1; }
            ')' if !in_single && !in_double => { paren_depth -= 1; }
            '{' if !in_single && !in_double => { brace_depth += 1; }
            '}' if !in_single && !in_double => { brace_depth -= 1; }
            '\\' if !in_single => { i += 1; } // Skip next
            _ => {}
        }
        i += 1;
    }

    in_single || in_double || paren_depth > 0 || brace_depth > 0
}
