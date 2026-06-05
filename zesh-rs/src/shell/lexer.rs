// Tokenizer for the shell binary

use crate::shell::types::{Token, TokKind};

pub struct Lexer {
    input: Vec<char>,
    pos: usize,
    pub line: u32,
}

impl Lexer {
    pub fn new(input: &str, start_line: u32) -> Self {
        Lexer {
            input: input.chars().collect(),
            pos: 0,
            line: start_line,
        }
    }

    fn peek(&self) -> Option<char> {
        self.input.get(self.pos).copied()
    }

    fn peek2(&self) -> Option<char> {
        self.input.get(self.pos + 1).copied()
    }

    fn advance(&mut self) -> Option<char> {
        let c = self.input.get(self.pos).copied();
        if c == Some('\n') {
            self.line += 1;
        }
        self.pos += 1;
        c
    }

    fn skip_whitespace(&mut self) {
        while let Some(c) = self.peek() {
            if c == ' ' || c == '\t' || c == '\r' {
                self.advance();
            } else if c == '\\' && self.peek2() == Some('\n') {
                // Line continuation
                self.advance(); // \
                self.advance(); // \n
            } else {
                break;
            }
        }
    }

    fn skip_comment(&mut self) {
        while let Some(c) = self.peek() {
            if c == '\n' {
                break;
            }
            self.advance();
        }
    }

    fn read_single_quoted(&mut self) -> String {
        // Already consumed opening '
        let mut s = String::from("'");
        loop {
            match self.advance() {
                None => {
                    s.push('\'');
                    break;
                }
                Some('\'') => {
                    s.push('\'');
                    break;
                }
                Some(c) => s.push(c),
            }
        }
        s
    }

    fn read_dollar_single_quoted(&mut self) -> String {
        // $'\n' etc - already consumed $'
        let mut result = String::new();
        loop {
            match self.advance() {
                None => break,
                Some('\'') => break,
                Some('\\') => {
                    match self.advance() {
                        Some('n')  => result.push('\n'),
                        Some('t')  => result.push('\t'),
                        Some('r')  => result.push('\r'),
                        Some('a')  => result.push('\x07'),
                        Some('b')  => result.push('\x08'),
                        Some('f')  => result.push('\x0c'),
                        Some('v')  => result.push('\x0b'),
                        Some('e') | Some('E') => result.push('\x1b'),
                        Some('\\') => result.push('\\'),
                        Some('\'') => result.push('\''),
                        Some('"')  => result.push('"'),
                        Some('?')  => result.push('?'),
                        Some('x') => {
                            let mut hex = String::new();
                            for _ in 0..2 {
                                if let Some(h) = self.peek() {
                                    if h.is_ascii_hexdigit() {
                                        hex.push(h);
                                        self.advance();
                                    } else {
                                        break;
                                    }
                                }
                            }
                            if !hex.is_empty() {
                                if let Ok(n) = u32::from_str_radix(&hex, 16) {
                                    if let Some(c) = char::from_u32(n) {
                                        result.push(c);
                                    }
                                }
                            }
                        }
                        Some('u') => {
                            let mut hex = String::new();
                            for _ in 0..4 {
                                if let Some(h) = self.peek() {
                                    if h.is_ascii_hexdigit() {
                                        hex.push(h);
                                        self.advance();
                                    } else {
                                        break;
                                    }
                                }
                            }
                            if !hex.is_empty() {
                                if let Ok(n) = u32::from_str_radix(&hex, 16) {
                                    if let Some(c) = char::from_u32(n) {
                                        result.push(c);
                                    }
                                }
                            }
                        }
                        Some(c) if c >= '0' && c <= '7' => {
                            let mut oct = String::new();
                            oct.push(c);
                            for _ in 0..2 {
                                if let Some(h) = self.peek() {
                                    if h >= '0' && h <= '7' {
                                        oct.push(h);
                                        self.advance();
                                    } else {
                                        break;
                                    }
                                }
                            }
                            if let Ok(n) = u32::from_str_radix(&oct, 8) {
                                if let Some(c) = char::from_u32(n) {
                                    result.push(c);
                                }
                            }
                        }
                        Some(c) => {
                            result.push('\\');
                            result.push(c);
                        }
                        None => break,
                    }
                }
                Some(c) => result.push(c),
            }
        }
        result
    }

    fn read_double_quoted(&mut self) -> String {
        // Already consumed opening "
        // Returns content, with nested expansions preserved as-is
        let mut s = String::new();
        loop {
            match self.peek() {
                None => break,
                Some('"') => {
                    self.advance();
                    break;
                }
                Some('\\') => {
                    self.advance();
                    match self.peek() {
                        Some(nc @ ('"' | '\\' | '$' | '`' | '\n')) => {
                            let nc = nc;
                            self.advance();
                            if nc != '\n' {
                                s.push('\\');
                                s.push(nc);
                            }
                        }
                        _ => {
                            s.push('\\');
                        }
                    }
                }
                Some('$') => {
                    // Preserve $ expansions for expand_string to handle
                    s.push(self.advance().unwrap());
                    // Read the expansion content
                    if let Some(next) = self.peek() {
                        match next {
                            '{' => {
                                s.push(self.advance().unwrap());
                                let body = self.read_brace_expansion();
                                s.push_str(&body);
                                s.push('}');
                            }
                            '(' => {
                                s.push(self.advance().unwrap());
                                if self.peek() == Some('(') {
                                    s.push(self.advance().unwrap());
                                    let body = self.read_double_paren();
                                    s.push_str(&body);
                                    s.push_str("))");
                                } else {
                                    let body = self.read_paren_body();
                                    s.push_str(&body);
                                    s.push(')');
                                }
                            }
                            '\'' => {
                                // $'...' inside double quotes
                                self.advance();
                                let escaped = self.read_dollar_single_quoted();
                                s.push_str(&escaped);
                            }
                            _ => {
                                // $VAR or $# etc - read identifier
                                let rest = self.read_var_name_in_dq();
                                s.push_str(&rest);
                            }
                        }
                    }
                }
                Some('`') => {
                    s.push(self.advance().unwrap());
                    let body = self.read_backtick_body();
                    s.push_str(&body);
                    s.push('`');
                }
                Some(_) => {
                    s.push(self.advance().unwrap());
                }
            }
        }
        s
    }

    fn read_var_name_in_dq(&mut self) -> String {
        let mut s = String::new();
        // Special chars
        if let Some(c) = self.peek() {
            match c {
                '@' | '*' | '#' | '?' | '$' | '!' | '0'..='9' | '_' | 'a'..='z' | 'A'..='Z' => {
                    s.push(self.advance().unwrap());
                    if c.is_alphanumeric() || c == '_' {
                        while let Some(nc) = self.peek() {
                            if nc.is_alphanumeric() || nc == '_' {
                                s.push(self.advance().unwrap());
                            } else {
                                break;
                            }
                        }
                    }
                }
                _ => {}
            }
        }
        s
    }

    fn read_brace_expansion(&mut self) -> String {
        // Read until matching }
        let mut s = String::new();
        let mut depth = 1;
        loop {
            match self.peek() {
                None => break,
                Some('{') => {
                    depth += 1;
                    s.push(self.advance().unwrap());
                }
                Some('}') => {
                    depth -= 1;
                    if depth == 0 {
                        self.advance(); // consume }
                        break;
                    }
                    s.push(self.advance().unwrap());
                }
                Some('\'') => {
                    s.push(self.advance().unwrap());
                    loop {
                        match self.advance() {
                            None | Some('\'') => { s.push('\''); break; }
                            Some(c) => s.push(c),
                        }
                    }
                }
                Some('"') => {
                    s.push(self.advance().unwrap());
                    loop {
                        match self.advance() {
                            None | Some('"') => { s.push('"'); break; }
                            Some('\\') => { s.push('\\'); if let Some(c) = self.advance() { s.push(c); } }
                            Some(c) => s.push(c),
                        }
                    }
                }
                Some(_) => {
                    s.push(self.advance().unwrap());
                }
            }
        }
        s
    }

    fn read_paren_body(&mut self) -> String {
        let mut s = String::new();
        let mut depth = 1;
        loop {
            match self.peek() {
                None => break,
                Some('(') => { depth += 1; s.push(self.advance().unwrap()); }
                Some(')') => {
                    depth -= 1;
                    if depth == 0 {
                        self.advance(); // consume )
                        break;
                    }
                    s.push(self.advance().unwrap());
                }
                Some('\'') => {
                    s.push(self.advance().unwrap());
                    loop {
                        match self.advance() {
                            None | Some('\'') => { s.push('\''); break; }
                            Some(c) => s.push(c),
                        }
                    }
                }
                Some('"') => {
                    s.push(self.advance().unwrap());
                    loop {
                        match self.advance() {
                            None | Some('"') => { s.push('"'); break; }
                            Some('\\') => { s.push('\\'); if let Some(c) = self.advance() { s.push(c); } }
                            Some(c) => s.push(c),
                        }
                    }
                }
                Some('\\') => {
                    s.push(self.advance().unwrap());
                    if let Some(c) = self.advance() { s.push(c); }
                }
                Some(_) => { s.push(self.advance().unwrap()); }
            }
        }
        s
    }

    fn read_double_paren(&mut self) -> String {
        // Read until matching ))
        let mut s = String::new();
        let mut depth = 2;
        loop {
            match self.peek() {
                None => break,
                Some('(') => { depth += 1; s.push(self.advance().unwrap()); }
                Some(')') => {
                    if depth == 2 {
                        // Check for ))
                        if self.input.get(self.pos + 1) == Some(&')') {
                            self.advance(); // first )
                            self.advance(); // second )
                            break;
                        }
                    }
                    depth -= 1;
                    s.push(self.advance().unwrap());
                }
                Some(_) => { s.push(self.advance().unwrap()); }
            }
        }
        s
    }

    fn read_backtick_body(&mut self) -> String {
        let mut s = String::new();
        loop {
            match self.peek() {
                None => break,
                Some('`') => {
                    self.advance();
                    break;
                }
                Some('\\') => {
                    s.push(self.advance().unwrap());
                    if let Some(c) = self.advance() { s.push(c); }
                }
                Some(_) => { s.push(self.advance().unwrap()); }
            }
        }
        s
    }

    fn read_word_raw(&mut self) -> (String, bool) {
        // Read a word token, handling quoting. Returns (value, quoted)
        let mut value = String::new();
        let mut quoted = false;

        loop {
            match self.peek() {
                None => break,
                Some(c) if is_word_break(c) => break,
                Some('\'') => {
                    quoted = true;
                    self.advance();
                    let sq = self.read_single_quoted();
                    value.push_str(&sq);
                }
                Some('$') => {
                    // Check for $'...' ANSI-C quoting
                    if self.peek2() == Some('\'') {
                        self.advance(); // $
                        self.advance(); // '
                        let s = self.read_dollar_single_quoted();
                        quoted = true; // treat as quoted (no word splitting)
                        value.push_str(&s);
                    } else {
                        // Keep $ and following expansion content
                        value.push(self.advance().unwrap()); // $
                        match self.peek() {
                            Some('{') => {
                                value.push(self.advance().unwrap()); // {
                                let body = self.read_brace_expansion();
                                value.push_str(&body);
                                value.push('}');
                            }
                            Some('(') => {
                                value.push(self.advance().unwrap()); // (
                                if self.peek() == Some('(') {
                                    value.push(self.advance().unwrap()); // second (
                                    let body = self.read_double_paren();
                                    value.push_str(&body);
                                    value.push_str("))");
                                } else {
                                    let body = self.read_paren_body();
                                    value.push_str(&body);
                                    value.push(')');
                                }
                            }
                            _ => {
                                // $VAR, $#, $$, etc
                                // read until word break
                                while let Some(c) = self.peek() {
                                    if c.is_alphanumeric() || c == '_' ||
                                       (value == "$" && matches!(c, '@' | '*' | '#' | '?' | '!' | '0'..='9')) {
                                        value.push(self.advance().unwrap());
                                    } else {
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                Some('"') => {
                    quoted = true;
                    self.advance(); // consume "
                    let dq = self.read_double_quoted();
                    // Wrap in double-quote markers
                    value.push('"');
                    value.push_str(&dq);
                    value.push('"');
                }
                Some('`') => {
                    self.advance();
                    let bt = self.read_backtick_body();
                    value.push('`');
                    value.push_str(&bt);
                    value.push('`');
                }
                Some('\\') => {
                    self.advance();
                    match self.peek() {
                        Some('\n') => {
                            self.advance(); // line continuation
                        }
                        Some(c) => {
                            let c = c;
                            self.advance();
                            value.push('\\');
                            value.push(c);
                        }
                        None => {}
                    }
                }
                Some(c) => {
                    value.push(c);
                    self.advance();
                }
            }
        }
        (value, quoted)
    }

    pub fn tokenize(&mut self) -> Vec<Token> {
        let mut tokens = Vec::new();

        loop {
            self.skip_whitespace();

            let line = self.line;

            match self.peek() {
                None => {
                    tokens.push(Token { kind: TokKind::Eof, value: String::new(), quoted: false, line });
                    break;
                }
                Some('#') => {
                    self.skip_comment();
                }
                Some('\n') => {
                    self.advance();
                    tokens.push(Token { kind: TokKind::Newline, value: "\n".to_string(), quoted: false, line });
                }
                Some(';') => {
                    self.advance();
                    if self.peek() == Some(';') {
                        self.advance();
                        tokens.push(Token { kind: TokKind::Semi, value: ";;".to_string(), quoted: false, line });
                    } else {
                        tokens.push(Token { kind: TokKind::Semi, value: ";".to_string(), quoted: false, line });
                    }
                }
                Some('|') => {
                    self.advance();
                    if self.peek() == Some('|') {
                        self.advance();
                        tokens.push(Token { kind: TokKind::Or, value: "||".to_string(), quoted: false, line });
                    } else if self.peek() == Some('&') {
                        self.advance();
                        tokens.push(Token { kind: TokKind::PipeErr, value: "|&".to_string(), quoted: false, line });
                    } else {
                        tokens.push(Token { kind: TokKind::Pipe, value: "|".to_string(), quoted: false, line });
                    }
                }
                Some('&') => {
                    self.advance();
                    if self.peek() == Some('&') {
                        self.advance();
                        tokens.push(Token { kind: TokKind::And, value: "&&".to_string(), quoted: false, line });
                    } else {
                        tokens.push(Token { kind: TokKind::Bg, value: "&".to_string(), quoted: false, line });
                    }
                }
                Some('(') => {
                    self.advance();
                    tokens.push(Token { kind: TokKind::LParen, value: "(".to_string(), quoted: false, line });
                }
                Some(')') => {
                    self.advance();
                    tokens.push(Token { kind: TokKind::RParen, value: ")".to_string(), quoted: false, line });
                }
                Some('{') => {
                    self.advance();
                    tokens.push(Token { kind: TokKind::LBrace, value: "{".to_string(), quoted: false, line });
                }
                Some('}') => {
                    self.advance();
                    tokens.push(Token { kind: TokKind::RBrace, value: "}".to_string(), quoted: false, line });
                }
                Some('!') => {
                    self.advance();
                    tokens.push(Token { kind: TokKind::Bang, value: "!".to_string(), quoted: false, line });
                }
                Some('<') => {
                    self.advance();
                    if self.peek() == Some('<') {
                        self.advance();
                        if self.peek() == Some('<') {
                            self.advance();
                            tokens.push(Token { kind: TokKind::RedirHerestr, value: "<<<".to_string(), quoted: false, line });
                        } else if self.peek() == Some('-') {
                            self.advance();
                            tokens.push(Token { kind: TokKind::RedirHeredoc, value: "<<-".to_string(), quoted: false, line });
                        } else {
                            tokens.push(Token { kind: TokKind::RedirHeredoc, value: "<<".to_string(), quoted: false, line });
                        }
                    } else if self.peek() == Some('&') {
                        self.advance();
                        // <& - read target
                        self.skip_whitespace();
                        let target = self.read_redir_target();
                        let val = format!("0&{}", target);
                        tokens.push(Token { kind: TokKind::RedirDupIn, value: val, quoted: false, line });
                    } else if self.peek() == Some('(') {
                        self.advance();
                        let body = self.read_proc_subst();
                        tokens.push(Token { kind: TokKind::Word, value: format!("<({body})"), quoted: false, line });
                    } else {
                        tokens.push(Token { kind: TokKind::RedirIn, value: "<".to_string(), quoted: false, line });
                    }
                }
                Some('>') => {
                    self.advance();
                    if self.peek() == Some('>') {
                        self.advance();
                        tokens.push(Token { kind: TokKind::RedirAppend, value: ">>".to_string(), quoted: false, line });
                    } else if self.peek() == Some('&') {
                        self.advance();
                        // >& - read target
                        self.skip_whitespace();
                        let target = self.read_redir_target();
                        let val = format!("1&{}", target);
                        tokens.push(Token { kind: TokKind::RedirDupOut, value: val, quoted: false, line });
                    } else if self.peek() == Some('(') {
                        self.advance();
                        let body = self.read_proc_subst();
                        tokens.push(Token { kind: TokKind::Word, value: format!(">({body})"), quoted: false, line });
                    } else {
                        tokens.push(Token { kind: TokKind::RedirOut, value: ">".to_string(), quoted: false, line });
                    }
                }
                Some(c) if c.is_ascii_digit() => {
                    // May be N> or N< or N>> or N>& or N<& or just a number
                    let mut num = String::new();
                    while let Some(d) = self.peek() {
                        if d.is_ascii_digit() {
                            num.push(d);
                            self.advance();
                        } else {
                            break;
                        }
                    }
                    match self.peek() {
                        Some('>') => {
                            self.advance();
                            if self.peek() == Some('>') {
                                self.advance();
                                tokens.push(Token { kind: TokKind::RedirFdAppend, value: num, quoted: false, line });
                            } else if self.peek() == Some('&') {
                                self.advance();
                                // Read target
                                let target = self.read_redir_target();
                                let val = if target.is_empty() {
                                    // No inline target - will be next token
                                    format!("{}", num)
                                } else {
                                    format!("{}&{}", num, target)
                                };
                                tokens.push(Token { kind: TokKind::RedirDupOut, value: val, quoted: false, line });
                            } else {
                                tokens.push(Token { kind: TokKind::RedirFdOut, value: num, quoted: false, line });
                            }
                        }
                        Some('<') => {
                            self.advance();
                            if self.peek() == Some('&') {
                                self.advance();
                                let target = self.read_redir_target();
                                let val = if target.is_empty() {
                                    format!("{}", num)
                                } else {
                                    format!("{}&{}", num, target)
                                };
                                tokens.push(Token { kind: TokKind::RedirDupIn, value: val, quoted: false, line });
                            } else {
                                tokens.push(Token { kind: TokKind::RedirFdIn, value: num, quoted: false, line });
                            }
                        }
                        _ => {
                            // Just a word starting with digits - continue reading
                            let (rest, quoted) = self.read_word_raw();
                            let full = format!("{}{}", num, rest);
                            let kind = word_to_keyword(&full);
                            tokens.push(Token { kind, value: full, quoted, line });
                        }
                    }
                }
                _ => {
                    // Read a word
                    let (value, quoted) = self.read_word_raw();
                    if value.is_empty() {
                        self.advance(); // Safety: skip unknown char
                        continue;
                    }
                    let kind = word_to_keyword(&value);
                    tokens.push(Token { kind, value, quoted, line });
                }
            }
        }

        // Now process heredocs
        self.process_heredocs(&mut tokens);

        tokens
    }

    fn read_proc_subst(&mut self) -> String {
        let mut depth = 1;
        let mut body = String::new();
        loop {
            match self.advance() {
                None => break,
                Some('(') => {
                    depth += 1;
                    body.push('(');
                }
                Some(')') => {
                    depth -= 1;
                    if depth == 0 {
                        break;
                    }
                    body.push(')');
                }
                Some('\\') => {
                    body.push('\\');
                    if let Some(c) = self.advance() {
                        body.push(c);
                    }
                }
                Some('\'') => {
                    body.push('\'');
                    loop {
                        match self.advance() {
                            None | Some('\'') => {
                                body.push('\'');
                                break;
                            }
                            Some(c) => body.push(c),
                        }
                    }
                }
                Some('"') => {
                    body.push('"');
                    loop {
                        match self.advance() {
                            None | Some('"') => {
                                body.push('"');
                                break;
                            }
                            Some('\\') => {
                                body.push('\\');
                                if let Some(c) = self.advance() { body.push(c); }
                            }
                            Some(c) => body.push(c),
                        }
                    }
                }
                Some(c) => body.push(c),
            }
        }
        body
    }

    fn read_redir_target(&mut self) -> String {
        let mut s = String::new();
        if self.peek() == Some('-') {
            s.push('-');
            self.advance();
        } else {
            while let Some(c) = self.peek() {
                if c.is_ascii_digit() {
                    s.push(c);
                    self.advance();
                } else {
                    break;
                }
            }
        }
        s
    }

    fn process_heredocs(&mut self, tokens: &mut Vec<Token>) {
        let mut i = 0;
        while i < tokens.len() {
            if tokens[i].kind == TokKind::RedirHeredoc {
                if i + 1 < tokens.len() {
                    let delim_raw = tokens[i + 1].value.clone();
                    let strip_tabs = tokens[i].value == "<<-";
                    let content = self.collect_heredoc(&delim_raw, strip_tabs);
                    tokens[i + 1].value = format!("\x00HEREDOC\x00{}\x00{}", delim_raw, content);
                }
            }
            i += 1;
        }
    }

    fn collect_heredoc(&mut self, delim_raw: &str, strip_tabs: bool) -> String {
        // Strip quotes from delimiter for comparison
        let bare_delim: String = delim_raw.chars()
            .filter(|&c| c != '\'' && c != '"' && c != '\\')
            .collect();
        let bare_delim = bare_delim.trim();

        let mut content = String::new();
        let remaining: Vec<char> = self.input[self.pos..].iter().copied().collect();
        let remaining_str: String = remaining.iter().collect();

        let mut chars_consumed = 0;
        for line in remaining_str.split('\n') {
            chars_consumed += line.len() + 1; // +1 for \n

            let check = if strip_tabs {
                line.trim_start_matches('\t')
            } else {
                line
            };

            if check == bare_delim {
                break;
            }

            let actual_line = if strip_tabs {
                line.trim_start_matches('\t').to_string()
            } else {
                line.to_string()
            };
            content.push_str(&actual_line);
            content.push('\n');
        }

        self.pos += chars_consumed.min(remaining.len());
        content
    }
}

fn is_word_break(c: char) -> bool {
    // These characters end a word when encountered unquoted
    // { and } ARE word breaks (standalone brace group delimiters)
    matches!(c, ' ' | '\t' | '\n' | ';' | '&' | '|' | '<' | '>' | '(' | ')' | '{' | '}')
}

fn word_to_keyword(word: &str) -> TokKind {
    match word {
        "if"       => TokKind::If,
        "then"     => TokKind::Then,
        "else"     => TokKind::Else,
        "elif"     => TokKind::Elif,
        "fi"       => TokKind::Fi,
        "while"    => TokKind::While,
        "until"    => TokKind::Until,
        "do"       => TokKind::Do,
        "done"     => TokKind::Done,
        "for"      => TokKind::For,
        "in"       => TokKind::In,
        "case"     => TokKind::Case,
        "esac"     => TokKind::Esac,
        "select"   => TokKind::Select,
        "function" => TokKind::Function,
        "time"     => TokKind::Time,
        "coproc"   => TokKind::Coproc,
        _          => TokKind::Word,
    }
}

pub fn lex(input: &str) -> Vec<Token> {
    let mut lexer = Lexer::new(input, 1);
    lexer.tokenize()
}

pub fn lex_with_line(input: &str, start_line: u32) -> Vec<Token> {
    let mut lexer = Lexer::new(input, start_line);
    lexer.tokenize()
}
