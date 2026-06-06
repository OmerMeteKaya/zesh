// Word expansion

use std::collections::HashMap;

const MAX_ARITH_DEPTH: usize = 64;
const MAX_EXPAND_DEPTH: usize = 32;

// Simple arithmetic evaluator (for $(()) and declare -i)
pub fn eval_arith_simple(expr: &str) -> Result<i64, String> {
    let expr = expr.trim();
    // Use empty var context
    ARITH_VARS.with(|v| {
        *v.borrow_mut() = Some(std::collections::HashMap::new());
    });
    let result = eval_arith_expr(expr);
    ARITH_VARS.with(|v| {
        *v.borrow_mut() = None;
    });
    result
}

fn eval_arith_expr(expr: &str) -> Result<i64, String> {
    ARITH_DEPTH.with(|d| {
        *d.borrow_mut() = 0;
    });
    let tokens = arith_tokenize(expr)?;
    let mut pos = 0;
    let result = arith_parse_expr(&tokens, &mut pos)?;
    ARITH_DEPTH.with(|d| {
        *d.borrow_mut() = 0;
    });
    Ok(result)
}

#[derive(Debug, Clone, PartialEq)]
enum ATok {
    Num(i64),
    Var(String),
    Plus, Minus, Star, Slash, Percent, StarStar,
    Amp, Pipe, Caret, Tilde, Bang,
    AmpAmp, PipePipe,
    EqEq, BangEq,
    Lt, Gt, LtEq, GtEq,
    LShift, RShift,
    LParen, RParen,
    Question, Colon,
}

fn arith_tokenize(s: &str) -> Result<Vec<ATok>, String> {
    let mut tokens = Vec::new();
    let chars: Vec<char> = s.chars().collect();
    let mut i = 0;

    while i < chars.len() {
        match chars[i] {
            ' ' | '\t' | '\n' => { i += 1; }
            '0'..='9' => {
                let mut n = String::new();
                // Hex
                if chars[i] == '0' && i + 1 < chars.len() && (chars[i+1] == 'x' || chars[i+1] == 'X') {
                    i += 2;
                    while i < chars.len() && chars[i].is_ascii_hexdigit() {
                        n.push(chars[i]);
                        i += 1;
                    }
                    let v = i64::from_str_radix(&n, 16).unwrap_or(0);
                    tokens.push(ATok::Num(v));
                } else if chars[i] == '0' && i + 1 < chars.len() && chars[i+1].is_ascii_digit() {
                    // Octal
                    i += 1;
                    while i < chars.len() && chars[i] >= '0' && chars[i] <= '7' {
                        n.push(chars[i]);
                        i += 1;
                    }
                    let v = i64::from_str_radix(&n, 8).unwrap_or(0);
                    tokens.push(ATok::Num(v));
                } else {
                    while i < chars.len() && chars[i].is_ascii_digit() {
                        n.push(chars[i]);
                        i += 1;
                    }
                    let v: i64 = n.parse().unwrap_or(0);
                    tokens.push(ATok::Num(v));
                }
            }
            'a'..='z' | 'A'..='Z' | '_' => {
                let mut name = String::new();
                while i < chars.len() && (chars[i].is_alphanumeric() || chars[i] == '_') {
                    name.push(chars[i]);
                    i += 1;
                }
                tokens.push(ATok::Var(name));
            }
            '+' => {
                if i + 1 < chars.len() && chars[i+1] == '+' {
                    tokens.push(ATok::Plus); tokens.push(ATok::Plus);
                    i += 2;
                } else {
                    tokens.push(ATok::Plus);
                    i += 1;
                }
            }
            '-' => {
                if i + 1 < chars.len() && chars[i+1] == '-' {
                    tokens.push(ATok::Minus); tokens.push(ATok::Minus);
                    i += 2;
                } else {
                    tokens.push(ATok::Minus);
                    i += 1;
                }
            }
            '*' => {
                if i + 1 < chars.len() && chars[i+1] == '*' {
                    tokens.push(ATok::StarStar);
                    i += 2;
                } else {
                    tokens.push(ATok::Star);
                    i += 1;
                }
            }
            '/' => { tokens.push(ATok::Slash); i += 1; }
            '%' => { tokens.push(ATok::Percent); i += 1; }
            '&' => {
                if i + 1 < chars.len() && chars[i+1] == '&' {
                    tokens.push(ATok::AmpAmp); i += 2;
                } else {
                    tokens.push(ATok::Amp); i += 1;
                }
            }
            '|' => {
                if i + 1 < chars.len() && chars[i+1] == '|' {
                    tokens.push(ATok::PipePipe); i += 2;
                } else {
                    tokens.push(ATok::Pipe); i += 1;
                }
            }
            '^' => { tokens.push(ATok::Caret); i += 1; }
            '~' => { tokens.push(ATok::Tilde); i += 1; }
            '!' => {
                if i + 1 < chars.len() && chars[i+1] == '=' {
                    tokens.push(ATok::BangEq); i += 2;
                } else {
                    tokens.push(ATok::Bang); i += 1;
                }
            }
            '=' => {
                if i + 1 < chars.len() && chars[i+1] == '=' {
                    tokens.push(ATok::EqEq); i += 2;
                } else {
                    i += 1; // skip lone =
                }
            }
            '<' => {
                if i + 1 < chars.len() && chars[i+1] == '=' {
                    tokens.push(ATok::LtEq); i += 2;
                } else if i + 1 < chars.len() && chars[i+1] == '<' {
                    tokens.push(ATok::LShift); i += 2;
                } else {
                    tokens.push(ATok::Lt); i += 1;
                }
            }
            '>' => {
                if i + 1 < chars.len() && chars[i+1] == '=' {
                    tokens.push(ATok::GtEq); i += 2;
                } else if i + 1 < chars.len() && chars[i+1] == '>' {
                    tokens.push(ATok::RShift); i += 2;
                } else {
                    tokens.push(ATok::Gt); i += 1;
                }
            }
            '(' => { tokens.push(ATok::LParen); i += 1; }
            ')' => { tokens.push(ATok::RParen); i += 1; }
            '?' => { tokens.push(ATok::Question); i += 1; }
            ':' => { tokens.push(ATok::Colon); i += 1; }
            _ => { i += 1; }
        }
    }
    Ok(tokens)
}

fn arith_parse_expr(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let depth_exceeded = ARITH_DEPTH.with(|d| {
        let mut depth = d.borrow_mut();
        if *depth >= MAX_ARITH_DEPTH {
            return true;
        }
        *depth += 1;
        false
    });
    if depth_exceeded {
        return Err("Arithmetic nesting too deep".to_string());
    }
    let result = arith_parse_ternary(tokens, pos);
    ARITH_DEPTH.with(|d| {
        *d.borrow_mut() -= 1;
    });
    result
}

fn arith_parse_ternary(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let cond = arith_parse_or(tokens, pos)?;
    if *pos < tokens.len() && tokens[*pos] == ATok::Question {
        *pos += 1;
        let t = arith_parse_expr(tokens, pos)?;
        if *pos < tokens.len() && tokens[*pos] == ATok::Colon {
            *pos += 1;
        }
        let f = arith_parse_expr(tokens, pos)?;
        return Ok(if cond != 0 { t } else { f });
    }
    Ok(cond)
}

fn arith_parse_or(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let mut left = arith_parse_and(tokens, pos)?;
    while *pos < tokens.len() && tokens[*pos] == ATok::PipePipe {
        *pos += 1;
        let right = arith_parse_and(tokens, pos)?;
        left = if left != 0 || right != 0 { 1 } else { 0 };
    }
    Ok(left)
}

fn arith_parse_and(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let mut left = arith_parse_bitor(tokens, pos)?;
    while *pos < tokens.len() && tokens[*pos] == ATok::AmpAmp {
        *pos += 1;
        let right = arith_parse_bitor(tokens, pos)?;
        left = if left != 0 && right != 0 { 1 } else { 0 };
    }
    Ok(left)
}

fn arith_parse_bitor(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let mut left = arith_parse_bitxor(tokens, pos)?;
    while *pos < tokens.len() && tokens[*pos] == ATok::Pipe {
        *pos += 1;
        let right = arith_parse_bitxor(tokens, pos)?;
        left |= right;
    }
    Ok(left)
}

fn arith_parse_bitxor(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let mut left = arith_parse_bitand(tokens, pos)?;
    while *pos < tokens.len() && tokens[*pos] == ATok::Caret {
        *pos += 1;
        let right = arith_parse_bitand(tokens, pos)?;
        left ^= right;
    }
    Ok(left)
}

fn arith_parse_bitand(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let mut left = arith_parse_eq(tokens, pos)?;
    while *pos < tokens.len() && tokens[*pos] == ATok::Amp {
        *pos += 1;
        let right = arith_parse_eq(tokens, pos)?;
        left &= right;
    }
    Ok(left)
}

fn arith_parse_eq(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let mut left = arith_parse_cmp(tokens, pos)?;
    loop {
        if *pos < tokens.len() && tokens[*pos] == ATok::EqEq {
            *pos += 1;
            let right = arith_parse_cmp(tokens, pos)?;
            left = if left == right { 1 } else { 0 };
        } else if *pos < tokens.len() && tokens[*pos] == ATok::BangEq {
            *pos += 1;
            let right = arith_parse_cmp(tokens, pos)?;
            left = if left != right { 1 } else { 0 };
        } else {
            break;
        }
    }
    Ok(left)
}

fn arith_parse_cmp(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let mut left = arith_parse_shift(tokens, pos)?;
    loop {
        if *pos >= tokens.len() { break; }
        match tokens[*pos] {
            ATok::Lt   => { *pos += 1; let r = arith_parse_shift(tokens, pos)?; left = if left < r { 1 } else { 0 }; }
            ATok::Gt   => { *pos += 1; let r = arith_parse_shift(tokens, pos)?; left = if left > r { 1 } else { 0 }; }
            ATok::LtEq => { *pos += 1; let r = arith_parse_shift(tokens, pos)?; left = if left <= r { 1 } else { 0 }; }
            ATok::GtEq => { *pos += 1; let r = arith_parse_shift(tokens, pos)?; left = if left >= r { 1 } else { 0 }; }
            _ => break,
        }
    }
    Ok(left)
}

fn arith_parse_shift(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let mut left = arith_parse_add(tokens, pos)?;
    loop {
        if *pos >= tokens.len() { break; }
        match tokens[*pos] {
            ATok::LShift => { *pos += 1; let r = arith_parse_add(tokens, pos)?; left <<= r; }
            ATok::RShift => { *pos += 1; let r = arith_parse_add(tokens, pos)?; left >>= r; }
            _ => break,
        }
    }
    Ok(left)
}

fn arith_parse_add(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let mut left = arith_parse_mul(tokens, pos)?;
    loop {
        if *pos >= tokens.len() { break; }
        match tokens[*pos] {
            ATok::Plus  => { *pos += 1; let r = arith_parse_mul(tokens, pos)?; left = left.wrapping_add(r); }
            ATok::Minus => { *pos += 1; let r = arith_parse_mul(tokens, pos)?; left = left.wrapping_sub(r); }
            _ => break,
        }
    }
    Ok(left)
}

fn arith_parse_mul(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let mut left = arith_parse_pow(tokens, pos)?;
    loop {
        if *pos >= tokens.len() { break; }
        match tokens[*pos] {
            ATok::Star    => { *pos += 1; let r = arith_parse_pow(tokens, pos)?; left = left.wrapping_mul(r); }
            ATok::Slash   => { *pos += 1; let r = arith_parse_pow(tokens, pos)?; if r == 0 { return Err("division by zero".to_string()); } left /= r; }
            ATok::Percent => { *pos += 1; let r = arith_parse_pow(tokens, pos)?; if r == 0 { return Err("division by zero".to_string()); } left %= r; }
            _ => break,
        }
    }
    Ok(left)
}

fn arith_parse_pow(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    let base = arith_parse_unary(tokens, pos)?;
    if *pos < tokens.len() && tokens[*pos] == ATok::StarStar {
        *pos += 1;
        let exp = arith_parse_unary(tokens, pos)?;
        Ok(base.wrapping_pow(exp as u32))
    } else {
        Ok(base)
    }
}

fn arith_parse_unary(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    if *pos >= tokens.len() {
        return Ok(0);
    }
    match &tokens[*pos] {
        ATok::Minus => {
            *pos += 1;
            let v = arith_parse_unary(tokens, pos)?;
            Ok(-v)
        }
        ATok::Plus => {
            *pos += 1;
            arith_parse_unary(tokens, pos)
        }
        ATok::Tilde => {
            *pos += 1;
            let v = arith_parse_unary(tokens, pos)?;
            Ok(!v)
        }
        ATok::Bang => {
            *pos += 1;
            let v = arith_parse_unary(tokens, pos)?;
            Ok(if v == 0 { 1 } else { 0 })
        }
        _ => arith_parse_primary(tokens, pos),
    }
}

// Global vars reference for arith (only used when called from eval_arith_simple)
thread_local! {
    static ARITH_VARS: std::cell::RefCell<Option<std::collections::HashMap<String, String>>> =
        std::cell::RefCell::new(None);
    static ARITH_DEPTH: std::cell::RefCell<usize> = std::cell::RefCell::new(0);
    static EXPAND_DEPTH: std::cell::RefCell<usize> = std::cell::RefCell::new(0);
    // Set to true when ${var:?err} triggers
    pub static PARAM_ERROR: std::cell::RefCell<bool> = std::cell::RefCell::new(false);
    pub static PARAM_ASSIGN: std::cell::RefCell<Vec<(String, String)>> = std::cell::RefCell::new(Vec::new());
    // Last command substitution $() exit status
    pub static LAST_CMDSUB_STATUS: std::cell::RefCell<Option<i32>> = std::cell::RefCell::new(None);
}

pub fn take_cmdsub_status() -> Option<i32> {
    LAST_CMDSUB_STATUS.with(|s| s.borrow_mut().take())
}

pub fn take_param_error() -> bool {
    PARAM_ERROR.with(|e| {
        let v = *e.borrow();
        *e.borrow_mut() = false;
        v
    })
}

pub fn push_param_assign(name: String, value: String) {
    PARAM_ASSIGN.with(|a| {
        a.borrow_mut().push((name, value));
    });
}

pub fn take_param_assigns() -> Vec<(String, String)> {
    PARAM_ASSIGN.with(|a| {
        let v = a.borrow().clone();
        a.borrow_mut().clear();
        v
    })
}

fn arith_parse_primary(tokens: &[ATok], pos: &mut usize) -> Result<i64, String> {
    if *pos >= tokens.len() {
        return Ok(0);
    }
    match &tokens[*pos] {
        ATok::Num(n) => {
            let v = *n;
            *pos += 1;
            Ok(v)
        }
        ATok::Var(name) => {
            let name = name.clone();
            *pos += 1;
            // Look up variable from thread-local context
            let val = ARITH_VARS.with(|v| {
                if let Some(map) = &*v.borrow() {
                    map.get(&name).cloned().unwrap_or_default()
                } else {
                    String::new()
                }
            });
            let n: i64 = val.trim().parse().unwrap_or(0);
            Ok(n)
        }
        ATok::LParen => {
            *pos += 1;
            let v = arith_parse_expr(tokens, pos)?;
            if *pos < tokens.len() && tokens[*pos] == ATok::RParen {
                *pos += 1;
            }
            Ok(v)
        }
        _ => Ok(0),
    }
}

// Expand a word token, returning list of strings (after word splitting + glob)
pub fn expand_word(word: &str, quoted: bool, vars: &crate::shell::vars::VarStore, script_file: &str) -> Vec<String> {
    let expanded = expand_word_no_split(word, quoted, vars, script_file);

    if quoted {
        return vec![expanded];
    }

    // Word split
    let ifs = vars.get_str("IFS").unwrap_or_else(|| " \t\n".to_string());
    let parts = word_split(&expanded, &ifs);

    // Glob expansion
    let mut result = Vec::new();
    for part in parts {
        let globbed = glob_expand(&part);
        result.extend(globbed);
    }

    if result.is_empty() && !expanded.is_empty() {
        // Keep empty string if the expansion produced nothing useful
    }

    result
}

pub fn expand_word_no_split(word: &str, _quoted: bool, vars: &crate::shell::vars::VarStore, script_file: &str) -> String {
    expand_string(word, vars, script_file)
}

pub fn expand_string(s: &str, vars: &crate::shell::vars::VarStore, script_file: &str) -> String {
    let depth_exceeded = EXPAND_DEPTH.with(|d| {
        let mut depth = d.borrow_mut();
        if *depth >= MAX_EXPAND_DEPTH {
            return true;
        }
        *depth += 1;
        false
    });
    if depth_exceeded {
        return String::new();
    }

    let result = expand_string_inner(s, vars, script_file);

    EXPAND_DEPTH.with(|d| {
        *d.borrow_mut() -= 1;
    });
    result
}

fn expand_string_inner(s: &str, vars: &crate::shell::vars::VarStore, script_file: &str) -> String {
    let chars: Vec<char> = s.chars().collect();
    let mut result = String::new();
    let mut i = 0;

    while i < chars.len() {
        match chars[i] {
            '"' => {
                // Double-quoted section - expand $ and ` inside
                i += 1;
                while i < chars.len() && chars[i] != '"' {
                    match chars[i] {
                        '$' => {
                            let (expanded, consumed) = expand_dollar(&chars, i, vars, script_file);
                            result.push_str(&expanded);
                            i += consumed;
                        }
                        '`' => {
                            let (expanded, consumed) = expand_backtick(&chars, i, vars, script_file);
                            result.push_str(&expanded);
                            i += consumed;
                        }
                        '\\' => {
                            i += 1;
                            if i < chars.len() {
                                match chars[i] {
                                    '"' | '\\' | '$' | '`' | '\n' => {
                                        if chars[i] != '\n' {
                                            result.push(chars[i]);
                                        }
                                        i += 1;
                                    }
                                    _ => {
                                        result.push('\\');
                                    }
                                }
                            }
                        }
                        c => {
                            result.push(c);
                            i += 1;
                        }
                    }
                }
                if i < chars.len() { i += 1; } // closing "
            }
            '\\' => {
                i += 1;
                if i < chars.len() {
                    result.push(chars[i]);
                    i += 1;
                }
            }
            '$' => {
                let (expanded, consumed) = expand_dollar(&chars, i, vars, script_file);
                result.push_str(&expanded);
                i += consumed;
            }
            '`' => {
                let (expanded, consumed) = expand_backtick(&chars, i, vars, script_file);
                result.push_str(&expanded);
                i += consumed;
            }
            '~' => {
                // Tilde expansion at start or after :
                let tilde_result = expand_tilde(&chars, i);
                result.push_str(&tilde_result.0);
                i += tilde_result.1;
            }
            '\'' => {
                // Single quotes - literal content
                i += 1;
                while i < chars.len() && chars[i] != '\'' {
                    result.push(chars[i]);
                    i += 1;
                }
                if i < chars.len() { i += 1; } // closing '
            }
            c => {
                result.push(c);
                i += 1;
            }
        }
    }
    result
}

fn expand_tilde(chars: &[char], start: usize) -> (String, usize) {
    let mut i = start + 1; // skip ~
    let mut name = String::new();
    while i < chars.len() && chars[i] != '/' && chars[i] != ':' && chars[i] != ' ' && chars[i] != '\t' {
        name.push(chars[i]);
        i += 1;
    }
    let consumed = i - start;

    if name.is_empty() {
        // ~ alone -> $HOME
        let home = std::env::var("HOME").unwrap_or_else(|_| {
            // Try getpwuid
            get_home_dir_by_uid(unsafe { libc::getuid() })
        });
        return (home, consumed);
    }

    // ~username
    match get_home_dir_by_name(&name) {
        Some(dir) => (dir, consumed),
        None => {
            // Keep literal
            let mut literal = String::from("~");
            literal.push_str(&name);
            (literal, consumed)
        }
    }
}

fn get_home_dir_by_name(username: &str) -> Option<String> {
    use std::ffi::CString;
    let cname = CString::new(username).ok()?;
    // SAFETY: getpwnam is called with a valid C string pointer
    let pw = unsafe { libc::getpwnam(cname.as_ptr()) };
    if pw.is_null() {
        return None;
    }
    // SAFETY: pw_dir is a valid C string if pw is non-null
    let dir = unsafe {
        std::ffi::CStr::from_ptr((*pw).pw_dir)
            .to_string_lossy()
            .into_owned()
    };
    Some(dir)
}

fn get_home_dir_by_uid(uid: u32) -> String {
    // SAFETY: getpwuid is called with a valid uid
    let pw = unsafe { libc::getpwuid(uid) };
    if pw.is_null() {
        return String::from("/");
    }
    // SAFETY: pw_dir is a valid C string if pw is non-null
    unsafe {
        std::ffi::CStr::from_ptr((*pw).pw_dir)
            .to_string_lossy()
            .into_owned()
    }
}

fn expand_dollar(chars: &[char], start: usize, vars: &crate::shell::vars::VarStore, script_file: &str) -> (String, usize) {
    let mut i = start + 1; // skip $
    if i >= chars.len() {
        return ("$".to_string(), 1);
    }

    match chars[i] {
        '{' => {
            // ${...}
            i += 1;
            let (result, consumed_from_open) = expand_brace(&chars[i..], vars, script_file);
            (result, 2 + consumed_from_open) // $ + { + content
        }
        '(' => {
            if i + 1 < chars.len() && chars[i+1] == '(' {
                // $(( arith ))
                i += 2;
                let (expr, consumed) = read_until_double_paren(&chars[i..]);
                let val = match eval_arith_expr_with_vars(&expr, vars) {
                    Ok(n) => n.to_string(),
                    Err(_) => "0".to_string(),
                };
                (val, 3 + consumed) // $, (, (, content, ), )
            } else {
                // $( cmd )
                i += 1;
                let (body, consumed) = read_until_close_paren(&chars[i..]);
                let output = run_command_substitution(&body, vars, script_file);
                let output = output.trim_end_matches('\n').to_string();
                (output, 2 + consumed) // $, (, content, )
            }
        }
        '\'' => {
            // $'...' - already handled in lexer, but in case we see it here
            i += 1;
            let mut result = String::new();
            while i < chars.len() && chars[i] != '\'' {
                if chars[i] == '\\' && i + 1 < chars.len() {
                    i += 1;
                    match chars[i] {
                        'n' => result.push('\n'),
                        't' => result.push('\t'),
                        'r' => result.push('\r'),
                        '\\' => result.push('\\'),
                        '\'' => result.push('\''),
                        _ => { result.push('\\'); result.push(chars[i]); }
                    }
                } else {
                    result.push(chars[i]);
                }
                i += 1;
            }
            let consumed = i - start + 1; // +1 for closing '
            (result, consumed)
        }
        '@' | '*' => {
            i += 1;
            // Expand positional parameters
            let pos_params = vars.get_str("@").unwrap_or_default();
            (pos_params, i - start)
        }
        '#' => {
            i += 1;
            if i < chars.len() && (chars[i].is_alphanumeric() || chars[i] == '_') {
                // ${#VAR} - but this is $# case
                // Actually $# is parameter count
                let count = vars.get_str("#").unwrap_or_else(|| "0".to_string());
                (count, i - start)
            } else {
                let count = vars.get_str("#").unwrap_or_else(|| "0".to_string());
                (count, i - start)
            }
        }
        '?' => {
            i += 1;
            let status = vars.get_str("?").unwrap_or_else(|| "0".to_string());
            (status, i - start)
        }
        '$' => {
            i += 1;
            let pid = vars.get_str("$").unwrap_or_else(|| {
                #[cfg(feature = "fuzz")]
                { "99999".to_string() }
                #[cfg(not(feature = "fuzz"))]
                {
                    std::process::id().to_string()
                }
            });
            (pid, i - start)
        }
        '!' => {
            i += 1;
            let bg_pid = {
                #[cfg(feature = "fuzz")]
                { "99998".to_string() }
                #[cfg(not(feature = "fuzz"))]
                {
                    vars.get_str("!").unwrap_or_default()
                }
            };
            (bg_pid, i - start)
        }
        '0' => {
            i += 1;
            let v = vars.get_str("0").unwrap_or_default();
            (v, i - start)
        }
        '1'..='9' => {
            let n = (chars[i] as u8 - b'0') as usize;
            i += 1;
            let v = vars.get_str(&n.to_string()).unwrap_or_default();
            (v, i - start)
        }
        '_' | 'a'..='z' | 'A'..='Z' => {
            let mut name = String::new();
            while i < chars.len() && (chars[i].is_alphanumeric() || chars[i] == '_') {
                name.push(chars[i]);
                i += 1;
            }
            let val = expand_special_var(&name, vars, script_file);
            (val, i - start)
        }
        _ => {
            ("$".to_string(), 1)
        }
    }
}

fn expand_special_var(name: &str, vars: &crate::shell::vars::VarStore, _script_file: &str) -> String {
    match name {
        "RANDOM" => {
            #[cfg(feature = "fuzz")]
            { "42".to_string() }
            #[cfg(not(feature = "fuzz"))]
            {
                let r = unsafe { libc::rand() } as u32 & 0x7fff;
                r.to_string()
            }
        }
        "SECONDS" => {
            #[cfg(feature = "fuzz")]
            { "42".to_string() }
            #[cfg(not(feature = "fuzz"))]
            {
                vars.get_str("SECONDS").unwrap_or_else(|| "0".to_string())
            }
        }
        "LINENO" => {
            vars.get_str("LINENO").unwrap_or_else(|| "0".to_string())
        }
        "FUNCNAME" => {
            vars.get_str("FUNCNAME").unwrap_or_default()
        }
        "BASH_SOURCE" => {
            vars.get_str("BASH_SOURCE").unwrap_or_default()
        }
        _ => {
            vars.get_str(name).unwrap_or_default()
        }
    }
}

fn expand_brace(chars: &[char], vars: &crate::shell::vars::VarStore, script_file: &str) -> (String, usize) {
    // Find matching }
    let mut depth = 1;
    let mut i = 0;
    while i < chars.len() {
        match chars[i] {
            '{' => depth += 1,
            '}' => {
                depth -= 1;
                if depth == 0 {
                    break;
                }
            }
            '\'' => {
                i += 1;
                while i < chars.len() && chars[i] != '\'' { i += 1; }
            }
            '"' => {
                i += 1;
                while i < chars.len() && chars[i] != '"' {
                    // Avoid skipping past end when backslash is the last char
                    if chars[i] == '\\' && i + 1 < chars.len() { i += 1; }
                    i += 1;
                }
            }
            _ => {}
        }
        // Guard: inner loops for ' and " can leave i at or past end
        if i >= chars.len() { break; }
        i += 1;
    }

    let i = i.min(chars.len()); // defensive cap in case of unterminated quote/brace
    let content: String = chars[..i].iter().collect();
    let consumed = i + 1; // include the }

    let result = expand_param(&content, vars, script_file);
    (result, consumed)
}

fn expand_param(content: &str, vars: &crate::shell::vars::VarStore, script_file: &str) -> String {
    // Handle special parameter expansions
    // ${#VAR} - length
    if content.starts_with('#') {
        let name = &content[1..];
        // Check for array ${#ARR[@]}
        if name.ends_with("[@]") || name.ends_with("[*]") {
            let arr_name = &name[..name.len()-3];
            let len = vars.array_len(arr_name);
            return len.to_string();
        }
        let val = expand_special_var(name, vars, script_file);
        return val.len().to_string();
    }

    // ${VAR@transform} — only when @ is not inside [...]
    if let Some(at_pos) = content.rfind('@') {
        let before = &content[..at_pos];
        // Skip if @ is inside brackets (e.g. ${ARR[@]})
        if !before.ends_with('[') {
            let transform = &content[at_pos+1..];
            let val = get_var_value(before, vars, script_file);
            return match transform {
                "U" => val.to_uppercase(),
                "L" | "l" => val.to_lowercase(),
                "Q" | "q" => format!("'{}'", val.replace('\'', "'\\''")),
                "E" => val, // TODO: process escape sequences
                _ => val,
            };
        }
    }

    // ${VAR:offset:length} - substring
    if let Some(colon1) = find_colon_outside_parens(content) {
        let name = &content[..colon1];
        let rest = &content[colon1+1..];

        // Check for default/alternate operators
        if rest.starts_with('-') || rest.starts_with('=') || rest.starts_with('+') || rest.starts_with('?') {
            return expand_default_param(name, rest, vars, script_file);
        }

        // Substring: :offset or :offset:length
        let val = get_var_value(name, vars, script_file);
        let parts: Vec<&str> = rest.splitn(2, ':').collect();
        if let Ok(offset) = parts[0].trim().parse::<i64>() {
            let chars: Vec<char> = val.chars().collect();
            let len = chars.len() as i64;
            let start = if offset >= 0 {
                offset.min(len) as usize
            } else {
                (len + offset).max(0) as usize
            };
            if parts.len() > 1 {
                if let Ok(count) = parts[1].trim().parse::<i64>() {
                    let end = (start as i64 + count).min(len) as usize;
                    let end = end.max(start);
                    return chars[start..end].iter().collect();
                }
            }
            return chars[start..].iter().collect();
        }
    }

    // Check for ${VAR:-default} etc without colon
    if content.contains('-') || content.contains('=') || content.contains('+') || content.contains('?') {
        for (idx, c) in content.char_indices() {
            if matches!(c, '-' | '=' | '+' | '?') {
                let name = &content[..idx];
                if is_valid_var_name(name) {
                    let rest = &content[idx..];
                    return expand_default_param_no_colon(name, rest, vars, script_file);
                }
            }
        }
    }

    // Array expansion ${ARR[idx]} or ${ARR[@]}
    if let Some(bracket_pos) = content.find('[') {
        let name = &content[..bracket_pos];
        let rest = &content[bracket_pos+1..];
        if let Some(end) = rest.find(']') {
            let idx_str = &rest[..end];
            if idx_str == "@" || idx_str == "*" {
                // Expand all elements
                if let Some(arr) = vars.get_array(name) {
                    let mut keys: Vec<usize> = arr.keys().copied().collect();
                    keys.sort();
                    return keys.iter().map(|k| arr[k].clone()).collect::<Vec<_>>().join(" ");
                }
                return String::new();
            }
            // Arithmetic index
            let idx = match eval_arith_expr_with_vars(idx_str, vars) {
                Ok(n) => n as usize,
                Err(_) => 0,
            };
            if let Some(arr) = vars.get_array(name) {
                return arr.get(&idx).cloned().unwrap_or_default();
            }
            return String::new();
        }
    }

    // Pattern removal ${VAR#pat}, ${VAR##pat}, ${VAR%pat}, ${VAR%%pat}
    // ${VAR/pat/repl}, ${VAR//pat/repl}
    for (i, c) in content.char_indices() {
        match c {
            '#' if i > 0 => {
                let name = &content[..i];
                let rest = &content[i+1..];
                let val = get_var_value(name, vars, script_file);
                let (greedy, pat) = if rest.starts_with('#') {
                    (true, &rest[1..])
                } else {
                    (false, rest)
                };
                return strip_prefix_pattern(&val, pat, greedy);
            }
            '%' if i > 0 => {
                let name = &content[..i];
                let rest = &content[i+1..];
                let val = get_var_value(name, vars, script_file);
                let (greedy, pat) = if rest.starts_with('%') {
                    (true, &rest[1..])
                } else {
                    (false, rest)
                };
                return strip_suffix_pattern(&val, pat, greedy);
            }
            '/' if i > 0 => {
                let name = &content[..i];
                let rest = &content[i+1..];
                let val = get_var_value(name, vars, script_file);
                let (global, rest) = if rest.starts_with('/') {
                    (true, &rest[1..])
                } else {
                    (false, rest)
                };
                let parts: Vec<&str> = rest.splitn(2, '/').collect();
                let pat = parts[0];
                let repl = if parts.len() > 1 { parts[1] } else { "" };
                return replace_pattern(&val, pat, repl, global);
            }
            _ => {}
        }
    }

    // Simple variable
    get_var_value(content, vars, script_file)
}

fn find_colon_outside_parens(s: &str) -> Option<usize> {
    let mut depth = 0;
    for (i, c) in s.char_indices() {
        match c {
            '(' | '[' | '{' => depth += 1,
            ')' | ']' | '}' => { if depth > 0 { depth -= 1; } }
            ':' if depth == 0 => return Some(i),
            _ => {}
        }
    }
    None
}

fn expand_default_param(name: &str, rest: &str, vars: &crate::shell::vars::VarStore, script_file: &str) -> String {
    // rest is the part after the colon: "-word", "=word", "+word", "?word"
    // OR it could be ":-word" if passed with the colon (legacy calling convention)
    let (op_char, word) = if rest.starts_with(":-") || rest.starts_with(":=") ||
                             rest.starts_with(":+") || rest.starts_with(":?") {
        // Has colon prefix
        (&rest[1..2], &rest[2..])
    } else if rest.starts_with('-') || rest.starts_with('=') ||
              rest.starts_with('+') || rest.starts_with('?') {
        (&rest[..1], &rest[1..])
    } else {
        return get_var_value(name, vars, script_file);
    };

    let val = get_var_value(name, vars, script_file);
    let is_unset_or_null = val.is_empty();

    match op_char {
        "-" => {
            if is_unset_or_null {
                expand_string(word, vars, script_file)
            } else {
                val
            }
        }
        "=" => {
            if is_unset_or_null {
                let new_val = expand_string(word, vars, script_file);
                // Signal that we need to set this variable
                push_param_assign(name.to_string(), new_val.clone());
                new_val
            } else {
                val
            }
        }
        "+" => {
            if is_unset_or_null {
                String::new()
            } else {
                expand_string(word, vars, script_file)
            }
        }
        "?" => {
            if is_unset_or_null {
                let msg = expand_string(word, vars, script_file);
                eprintln!("zesh: {}: {}", name, msg);
                // Signal parameter error
                PARAM_ERROR.with(|e| *e.borrow_mut() = true);
                String::new()
            } else {
                val
            }
        }
        _ => val,
    }
}

fn expand_default_param_no_colon(name: &str, rest: &str, vars: &crate::shell::vars::VarStore, script_file: &str) -> String {
    let (op_char, word) = (&rest[..1], &rest[1..]);
    let val = vars.get_str(name);
    let is_unset = val.is_none();

    match op_char {
        "-" => {
            if is_unset {
                expand_string(word, vars, script_file)
            } else {
                val.unwrap_or_default()
            }
        }
        "=" => {
            if is_unset {
                expand_string(word, vars, script_file)
            } else {
                val.unwrap_or_default()
            }
        }
        "+" => {
            if is_unset {
                String::new()
            } else {
                expand_string(word, vars, script_file)
            }
        }
        "?" => {
            if is_unset {
                let msg = expand_string(word, vars, script_file);
                eprintln!("zesh: {}: {}", name, msg);
                String::new()
            } else {
                val.unwrap_or_default()
            }
        }
        _ => val.unwrap_or_default(),
    }
}

fn is_valid_var_name(s: &str) -> bool {
    if s.is_empty() { return false; }
    let mut chars = s.chars();
    let first = chars.next().unwrap();
    if !first.is_alphabetic() && first != '_' { return false; }
    chars.all(|c| c.is_alphanumeric() || c == '_')
}

fn get_var_value(name: &str, vars: &crate::shell::vars::VarStore, script_file: &str) -> String {
    // Returns the variable value, empty string if unset
    expand_special_var(name, vars, script_file)
}

fn is_var_unset(name: &str, vars: &crate::shell::vars::VarStore) -> bool {
    vars.get(name).is_none()
}

fn strip_prefix_pattern(val: &str, pat: &str, greedy: bool) -> String {
    if greedy {
        // Longest prefix match
        for end in (0..=val.len()).rev() {
            if let Some(slice) = val.get(..end) {
                if glob_match(pat, slice) {
                    return val[end..].to_string();
                }
            }
        }
    } else {
        // Shortest prefix match
        for end in 0..=val.len() {
            if let Some(slice) = val.get(..end) {
                if glob_match(pat, slice) {
                    return val[end..].to_string();
                }
            }
        }
    }
    val.to_string()
}

fn strip_suffix_pattern(val: &str, pat: &str, greedy: bool) -> String {
    if greedy {
        for start in 0..=val.len() {
            if let Some(slice) = val.get(start..) {
                if glob_match(pat, slice) {
                    return val[..start].to_string();
                }
            }
        }
    } else {
        for start in (0..=val.len()).rev() {
            if let Some(slice) = val.get(start..) {
                if glob_match(pat, slice) {
                    return val[..start].to_string();
                }
            }
        }
    }
    val.to_string()
}

fn replace_pattern(val: &str, pat: &str, repl: &str, global: bool) -> String {
    if global {
        // Replace all non-overlapping occurrences
        let mut result = String::new();
        let mut i = 0;
        let chars: Vec<char> = val.chars().collect();
        while i <= chars.len() {
            let mut matched = false;
            for end in (i..=chars.len()).rev() {
                let slice: String = chars[i..end].iter().collect();
                if glob_match(pat, &slice) {
                    result.push_str(repl);
                    if end == i {
                        // Empty-pattern match: consume the next char too so we
                        // don't loop forever (mirrors bash "${v///R}" behaviour).
                        if i < chars.len() {
                            result.push(chars[i]);
                        }
                        i += 1;
                    } else {
                        i = end;
                    }
                    matched = true;
                    break;
                }
            }
            if !matched {
                if i < chars.len() {
                    result.push(chars[i]);
                }
                i += 1;
            }
        }
        result
    } else {
        // Replace first occurrence
        let chars: Vec<char> = val.chars().collect();
        for start in 0..=chars.len() {
            for end in (start..=chars.len()).rev() {
                let slice: String = chars[start..end].iter().collect();
                if glob_match(pat, &slice) {
                    let before: String = chars[..start].iter().collect();
                    let after: String = chars[end..].iter().collect();
                    return format!("{}{}{}", before, repl, after);
                }
            }
        }
        val.to_string()
    }
}

pub fn glob_match(pattern: &str, s: &str) -> bool {
    glob_match_chars(
        &pattern.chars().collect::<Vec<_>>(),
        0,
        &s.chars().collect::<Vec<_>>(),
        0,
    )
}

fn glob_match_chars(pat: &[char], pi: usize, s: &[char], si: usize) -> bool {
    if pi >= pat.len() {
        return si >= s.len();
    }
    match pat[pi] {
        '*' => {
            // Try matching zero or more characters
            for ni in si..=s.len() {
                if glob_match_chars(pat, pi + 1, s, ni) {
                    return true;
                }
            }
            false
        }
        '?' => {
            si < s.len() && glob_match_chars(pat, pi + 1, s, si + 1)
        }
        '[' => {
            // Character class
            let mut pi2 = pi + 1;
            let negate = pi2 < pat.len() && pat[pi2] == '!';
            if negate { pi2 += 1; }
            let mut matched = false;
            let mut first = true;
            while pi2 < pat.len() && (first || pat[pi2] != ']') {
                first = false;
                if pi2 + 2 < pat.len() && pat[pi2 + 1] == '-' && pat[pi2 + 2] != ']' {
                    if si < s.len() && s[si] >= pat[pi2] && s[si] <= pat[pi2 + 2] {
                        matched = true;
                    }
                    pi2 += 3;
                } else {
                    if si < s.len() && s[si] == pat[pi2] {
                        matched = true;
                    }
                    pi2 += 1;
                }
            }
            if pi2 < pat.len() { pi2 += 1; } // skip ]
            let result = if negate { !matched } else { matched };
            result && si < s.len() && glob_match_chars(pat, pi2, s, si + 1)
        }
        '\\' if pi + 1 < pat.len() => {
            si < s.len() && s[si] == pat[pi + 1] && glob_match_chars(pat, pi + 2, s, si + 1)
        }
        c => {
            si < s.len() && s[si] == c && glob_match_chars(pat, pi + 1, s, si + 1)
        }
    }
}

fn expand_backtick(chars: &[char], start: usize, vars: &crate::shell::vars::VarStore, script_file: &str) -> (String, usize) {
    let mut i = start + 1; // skip `
    let mut body = String::new();
    while i < chars.len() {
        if chars[i] == '`' {
            i += 1;
            break;
        }
        if chars[i] == '\\' && i + 1 < chars.len() {
            body.push(chars[i+1]);
            i += 2;
        } else {
            body.push(chars[i]);
            i += 1;
        }
    }
    let output = run_command_substitution(&body, vars, script_file);
    let output = output.trim_end_matches('\n').to_string();
    (output, i - start)
}

fn read_until_double_paren(chars: &[char]) -> (String, usize) {
    let mut depth = 2; // we're inside ((
    let mut i = 0;
    let mut expr = String::new();
    while i < chars.len() {
        if chars[i] == ')' && i + 1 < chars.len() && chars[i+1] == ')' {
            depth -= 2;
            if depth <= 0 {
                i += 2;
                break;
            }
        } else if chars[i] == '(' {
            depth += 1;
            expr.push(chars[i]);
        } else if chars[i] == ')' {
            depth -= 1;
            expr.push(chars[i]);
        } else {
            expr.push(chars[i]);
        }
        i += 1;
    }
    (expr, i)
}

fn read_until_close_paren(chars: &[char]) -> (String, usize) {
    let mut depth = 1;
    let mut i = 0;
    let mut body = String::new();
    while i < chars.len() {
        match chars[i] {
            '(' => { depth += 1; body.push('('); }
            ')' => {
                depth -= 1;
                if depth == 0 {
                    i += 1;
                    break;
                }
                body.push(')');
            }
            '\'' => {
                body.push('\'');
                i += 1;
                while i < chars.len() && chars[i] != '\'' {
                    body.push(chars[i]);
                    i += 1;
                }
                if i < chars.len() { body.push('\''); }
            }
            '"' => {
                body.push('"');
                i += 1;
                while i < chars.len() && chars[i] != '"' {
                    if chars[i] == '\\' { body.push(chars[i]); i += 1; }
                    if i < chars.len() { body.push(chars[i]); }
                    i += 1;
                }
                if i < chars.len() { body.push('"'); }
            }
            c => { body.push(c); }
        }
        i += 1;
    }
    (body, i)
}

pub fn run_command_substitution(cmd: &str, vars: &crate::shell::vars::VarStore, script_file: &str) -> String {
    use std::os::unix::io::FromRawFd;
    use std::io::Read;

    // Create pipe
    let mut pipe_fds = [0i32; 2];
    // SAFETY: pipe() is a valid syscall
    if unsafe { libc::pipe(pipe_fds.as_mut_ptr()) } != 0 {
        return String::new();
    }

    // SAFETY: fork() is a valid syscall
    let pid = unsafe { libc::fork() };
    if pid < 0 {
        // SAFETY: closing valid file descriptors
        unsafe { libc::close(pipe_fds[0]); libc::close(pipe_fds[1]); }
        return String::new();
    }

    if pid == 0 {
        // Child
        // SAFETY: dup2 with valid fds
        unsafe {
            libc::close(pipe_fds[0]);
            libc::dup2(pipe_fds[1], 1);
            libc::close(pipe_fds[1]);
        }

        // Run the command
        let tokens = crate::shell::lexer::lex(cmd);
        let nodes = crate::shell::parser::parse(tokens);
        // Need to create a fresh context
        let mut ctx = crate::shell::executor::ExecContext::new_subshell();
        ctx.script_file = script_file.to_string();
        // Copy vars into subshell context
        // Actually we pass vars separately
        let status = crate::shell::executor::execute_list_with_vars(&nodes, &mut ctx, vars);
        // SAFETY: _exit is always safe to call
        unsafe { libc::_exit(status) };
    }

    // Parent
    // SAFETY: closing valid file descriptor
    unsafe { libc::close(pipe_fds[1]); }
    let mut file = unsafe { std::fs::File::from_raw_fd(pipe_fds[0]) };
    let mut output = String::new();
    let _ = file.read_to_string(&mut output);

    // Wait for child
    let mut status = 0;
    // SAFETY: waitpid with valid pid
    unsafe { libc::waitpid(pid, &mut status, 0); }
    let exit_code = if libc::WIFEXITED(status) { libc::WEXITSTATUS(status) } else { 1 };
    LAST_CMDSUB_STATUS.with(|s| *s.borrow_mut() = Some(exit_code));

    output
}

pub fn eval_arith_expr_with_vars(expr: &str, vars: &crate::shell::vars::VarStore) -> Result<i64, String> {
    // Expand $VAR references in the expression, then evaluate
    // Also set up thread-local var context for bare variable names
    let depth_exceeded = EXPAND_DEPTH.with(|d| {
        let mut depth = d.borrow_mut();
        if *depth >= MAX_EXPAND_DEPTH {
            return true;
        }
        *depth += 1;
        false
    });
    if depth_exceeded {
        return Err("Expansion nesting too deep".to_string());
    }
    let var_map = vars.all_vars();
    ARITH_VARS.with(|v| {
        *v.borrow_mut() = Some(var_map);
    });
    let expanded = expand_vars_in_arith(expr, vars);
    let result = eval_arith_expr(&expanded);
    ARITH_VARS.with(|v| {
        *v.borrow_mut() = None;
    });
    EXPAND_DEPTH.with(|d| {
        *d.borrow_mut() -= 1;
    });
    result
}

fn expand_vars_in_arith(expr: &str, vars: &crate::shell::vars::VarStore) -> String {
    let chars: Vec<char> = expr.chars().collect();
    let mut result = String::new();
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '$' {
            i += 1;
            if i < chars.len() && chars[i] == '(' {
                // Nested $(())
                if i + 1 < chars.len() && chars[i+1] == '(' {
                    i += 2;
                    let (inner_expr, consumed) = read_until_double_paren(&chars[i..]);
                    let val = match eval_arith_expr_with_vars(&inner_expr, vars) {
                        Ok(n) => n.to_string(),
                        Err(_) => "0".to_string(),
                    };
                    result.push_str(&val);
                    i += consumed;
                } else {
                    result.push('$');
                }
            } else if i < chars.len() && (chars[i].is_alphabetic() || chars[i] == '_') {
                let mut name = String::new();
                while i < chars.len() && (chars[i].is_alphanumeric() || chars[i] == '_') {
                    name.push(chars[i]);
                    i += 1;
                }
                let val = vars.get_str(&name).unwrap_or_default();
                let n: i64 = val.trim().parse().unwrap_or(0);
                result.push_str(&n.to_string());
            } else {
                result.push('$');
            }
        } else {
            result.push(chars[i]);
            i += 1;
        }
    }
    result
}

fn word_split(s: &str, ifs: &str) -> Vec<String> {
    if ifs.is_empty() {
        return vec![s.to_string()];
    }

    let ifs_whitespace: Vec<char> = ifs.chars().filter(|c| c.is_whitespace()).collect();
    let ifs_nonws: Vec<char> = ifs.chars().filter(|c| !c.is_whitespace()).collect();

    let mut parts = Vec::new();
    let mut current = String::new();
    let chars: Vec<char> = s.chars().collect();
    let mut i = 0;

    while i < chars.len() {
        let c = chars[i];
        if ifs_whitespace.contains(&c) {
            if !current.is_empty() {
                parts.push(current.clone());
                current.clear();
            }
            // Skip whitespace
            while i < chars.len() && ifs_whitespace.contains(&chars[i]) {
                i += 1;
            }
        } else if ifs_nonws.contains(&c) {
            if !current.is_empty() {
                parts.push(current.clone());
                current.clear();
            } else {
                parts.push(String::new());
            }
            i += 1;
            // Skip whitespace after non-whitespace IFS
            while i < chars.len() && ifs_whitespace.contains(&chars[i]) {
                i += 1;
            }
        } else {
            current.push(c);
            i += 1;
        }
    }
    if !current.is_empty() {
        parts.push(current);
    }
    parts
}

fn glob_expand(pattern: &str) -> Vec<String> {
    // Only glob if pattern contains glob chars
    if !pattern.contains('*') && !pattern.contains('?') && !pattern.contains('[') {
        return vec![pattern.to_string()];
    }

    match glob::glob(pattern) {
        Ok(paths) => {
            let mut results: Vec<String> = paths
                .filter_map(|p| p.ok())
                .map(|p| p.to_string_lossy().into_owned())
                .collect();
            if results.is_empty() {
                results.push(pattern.to_string());
            }
            results
        }
        Err(_) => vec![pattern.to_string()],
    }
}

// Expand a word token (from the lexer) - handles all the encoding
pub fn expand_token(tok: &crate::shell::types::Token, vars: &crate::shell::vars::VarStore, script_file: &str) -> Vec<String> {
    // Handle $'\n' etc - the lexer already processed these
    // The token value may contain raw chars if it was $'...' quoted

    // Check for process substitution
    if tok.value.starts_with("<(") || tok.value.starts_with(">(") {
        let is_input = tok.value.starts_with("<(");
        let end = if tok.value.ends_with(')') {
            tok.value.len() - 1
        } else {
            tok.value.len()
        };
        let cmd = if is_input { &tok.value[2..end] } else { &tok.value[2..end] };
        let path = create_process_substitution(cmd, vars, script_file, is_input);
        return vec![path];
    }

    let parts = expand_word(&tok.value, tok.quoted, vars, script_file);
    if parts.is_empty() && tok.quoted {
        vec![String::new()]
    } else {
        parts
    }
}

fn create_process_substitution(cmd: &str, vars: &crate::shell::vars::VarStore, script_file: &str, is_input: bool) -> String {
    let mut pipe_fds = [0i32; 2];
    // SAFETY: pipe() is a valid syscall
    if unsafe { libc::pipe(pipe_fds.as_mut_ptr()) } != 0 {
        return "/dev/null".to_string();
    }

    // SAFETY: fork() is a valid syscall
    let pid = unsafe { libc::fork() };
    if pid < 0 {
        // SAFETY: closing valid file descriptors
        unsafe { libc::close(pipe_fds[0]); libc::close(pipe_fds[1]); }
        return "/dev/null".to_string();
    }

    if pid == 0 {
        // Child
        if is_input {
            // Output goes to pipe write end
            // SAFETY: dup2 with valid fds
            unsafe {
                libc::close(pipe_fds[0]);
                libc::dup2(pipe_fds[1], 1);
                libc::close(pipe_fds[1]);
            }
        } else {
            // SAFETY: dup2 with valid fds
            unsafe {
                libc::close(pipe_fds[1]);
                libc::dup2(pipe_fds[0], 0);
                libc::close(pipe_fds[0]);
            }
        }
        let tokens = crate::shell::lexer::lex(cmd);
        let nodes = crate::shell::parser::parse(tokens);
        let mut ctx = crate::shell::executor::ExecContext::new_subshell();
        ctx.script_file = script_file.to_string();
        let status = crate::shell::executor::execute_list_with_vars(&nodes, &mut ctx, vars);
        // SAFETY: _exit is always safe
        unsafe { libc::_exit(status) };
    }

    // Parent
    if is_input {
        // SAFETY: close valid fd
        unsafe { libc::close(pipe_fds[1]); }
        format!("/dev/fd/{}", pipe_fds[0])
    } else {
        // SAFETY: close valid fd
        unsafe { libc::close(pipe_fds[0]); }
        format!("/dev/fd/{}", pipe_fds[1])
    }
}
