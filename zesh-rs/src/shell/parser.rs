// Parser: Token stream -> AST

use crate::shell::types::*;

pub struct Parser {
    tokens: Vec<Token>,
    pos: usize,
}

impl Parser {
    pub fn new(tokens: Vec<Token>) -> Self {
        Parser { tokens, pos: 0 }
    }

    fn peek(&self) -> &Token {
        static EOF: std::sync::OnceLock<Token> = std::sync::OnceLock::new();
        let eof = EOF.get_or_init(|| Token {
            kind: TokKind::Eof,
            value: String::new(),
            quoted: false,
            line: 0,
        });
        self.tokens.get(self.pos).unwrap_or(eof)
    }

    fn peek_kind(&self) -> &TokKind {
        &self.peek().kind
    }

    fn advance(&mut self) -> &Token {
        let t = &self.tokens[self.pos];
        if self.pos < self.tokens.len() - 1 {
            self.pos += 1;
        }
        t
    }

    fn consume(&mut self) -> Token {
        let t = self.tokens[self.pos].clone();
        if self.pos < self.tokens.len() - 1 {
            self.pos += 1;
        }
        t
    }

    fn skip_newlines(&mut self) {
        while matches!(self.peek_kind(), TokKind::Newline | TokKind::Semi) {
            self.advance();
        }
    }

    fn skip_semi_newline(&mut self) {
        while matches!(self.peek_kind(), TokKind::Newline) {
            self.advance();
        }
    }

    pub fn parse_list(&mut self) -> Vec<CmdNode> {
        let mut nodes = Vec::new();
        self.skip_newlines();

        while !matches!(self.peek_kind(), TokKind::Eof) {
            let node = self.parse_and_or();
            if let Some(mut node) = node {
                match self.peek_kind() {
                    TokKind::Semi => {
                        self.advance();
                        node.op = NodeOp::Semi;
                        self.skip_newlines();
                    }
                    TokKind::Bg => {
                        self.advance();
                        node.background = true;
                        node.op = NodeOp::Bg;
                        self.skip_newlines();
                    }
                    TokKind::Newline => {
                        self.advance();
                        node.op = NodeOp::Semi;
                        self.skip_newlines();
                    }
                    _ => {
                        node.op = NodeOp::End;
                    }
                }
                nodes.push(node);
            } else {
                break;
            }

            if matches!(self.peek_kind(), TokKind::Eof | TokKind::RParen | TokKind::RBrace) {
                break;
            }
        }
        nodes
    }

    fn parse_and_or(&mut self) -> Option<CmdNode> {
        let mut left = self.parse_pipeline()?;

        loop {
            match self.peek_kind() {
                TokKind::And => {
                    self.advance();
                    self.skip_newlines();
                    left.op = NodeOp::And;
                    let right = self.parse_pipeline()?;
                    // Wrap as a list
                    let line = left.lineno;
                    let nodes = vec![left, right];
                    left = CmdNode {
                        kind: CompoundKind::Brace(nodes),
                        redirs: vec![],
                        background: false,
                        negate: false,
                        op: NodeOp::End,
                        lineno: line,
                    };
                    // Actually: we keep it flat with op=And
                    // Let's redo this: keep left as is, then we return a "list" node
                    // Simpler: return AND node
                    break;
                }
                TokKind::Or => {
                    self.advance();
                    self.skip_newlines();
                    left.op = NodeOp::Or;
                    let right = self.parse_pipeline()?;
                    let line = left.lineno;
                    let nodes = vec![left, right];
                    left = CmdNode {
                        kind: CompoundKind::Brace(nodes),
                        redirs: vec![],
                        background: false,
                        negate: false,
                        op: NodeOp::End,
                        lineno: line,
                    };
                    break;
                }
                _ => break,
            }
        }

        Some(left)
    }

    fn parse_pipeline(&mut self) -> Option<CmdNode> {
        let negate = if self.peek_kind() == &TokKind::Bang {
            self.advance();
            true
        } else {
            false
        };

        // time keyword
        let has_time = if self.peek_kind() == &TokKind::Time {
            self.advance();
            true
        } else {
            false
        };

        let mut cmds = Vec::new();
        let first = self.parse_compound()?;
        let line = first.lineno;
        cmds.push(first);

        let mut is_pipe_err = false;
        loop {
            match self.peek_kind() {
                TokKind::Pipe => {
                    self.advance();
                    self.skip_newlines();
                    if let Some(c) = self.parse_compound() {
                        cmds.push(c);
                    }
                }
                TokKind::PipeErr => {
                    is_pipe_err = true;
                    self.advance();
                    self.skip_newlines();
                    if let Some(c) = self.parse_compound() {
                        cmds.push(c);
                    }
                }
                _ => break,
            }
        }

        let node = if cmds.len() == 1 && !has_time {
            let mut n = cmds.remove(0);
            n.negate = negate;
            n
        } else if has_time {
            CmdNode {
                kind: CompoundKind::Time(cmds),
                redirs: vec![],
                background: false,
                negate,
                op: NodeOp::End,
                lineno: line,
            }
        } else {
            CmdNode {
                kind: CompoundKind::Pipeline(cmds, is_pipe_err),
                redirs: vec![],
                background: false,
                negate,
                op: NodeOp::End,
                lineno: line,
            }
        };

        Some(node)
    }

    fn parse_compound(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;

        match self.peek_kind() {
            TokKind::LParen => {
                self.advance();
                self.skip_newlines();
                let body = self.parse_list();
                // Consume )
                if self.peek_kind() == &TokKind::RParen {
                    self.advance();
                }
                let redirs = self.parse_redirs();
                Some(CmdNode {
                    kind: CompoundKind::Subshell(body),
                    redirs,
                    background: false,
                    negate: false,
                    op: NodeOp::End,
                    lineno: line,
                })
            }
            TokKind::LBrace => {
                self.advance();
                self.skip_newlines();
                let body = self.parse_list();
                if self.peek_kind() == &TokKind::RBrace {
                    self.advance();
                }
                let redirs = self.parse_redirs();
                Some(CmdNode {
                    kind: CompoundKind::Brace(body),
                    redirs,
                    background: false,
                    negate: false,
                    op: NodeOp::End,
                    lineno: line,
                })
            }
            TokKind::If => self.parse_if(),
            TokKind::While => self.parse_while(),
            TokKind::Until => self.parse_until(),
            TokKind::For => self.parse_for(),
            TokKind::Case => self.parse_case(),
            TokKind::Select => self.parse_select(),
            TokKind::Function => self.parse_function_def(),
            TokKind::Coproc => self.parse_coproc(),
            TokKind::Word | TokKind::Assign => {
                // Could be function def: NAME() { ... }
                if self.is_function_def() {
                    self.parse_function_def_shorthand()
                } else {
                    self.parse_simple_command()
                }
            }
            TokKind::Bang => {
                self.advance();
                let inner = self.parse_compound()?;
                let line2 = inner.lineno;
                let mut n = CmdNode {
                    kind: CompoundKind::Brace(vec![inner]),
                    redirs: vec![],
                    background: false,
                    negate: true,
                    op: NodeOp::End,
                    lineno: line2,
                };
                n.negate = true;
                Some(n)
            }
            _ => None,
        }
    }

    fn is_function_def(&self) -> bool {
        // NAME followed by ( ) or NAME followed by ()
        let mut i = self.pos;
        // Skip word
        if i >= self.tokens.len() { return false; }
        if !matches!(self.tokens[i].kind, TokKind::Word) { return false; }
        i += 1;
        if i >= self.tokens.len() { return false; }
        if self.tokens[i].kind != TokKind::LParen { return false; }
        i += 1;
        if i >= self.tokens.len() { return false; }
        if self.tokens[i].kind != TokKind::RParen { return false; }
        true
    }

    fn parse_function_def_shorthand(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;
        let name = self.consume().value;
        // consume ( )
        self.advance(); // (
        self.advance(); // )
        self.skip_newlines();
        let body = self.parse_compound()?;
        Some(CmdNode {
            kind: CompoundKind::FuncDef { name, body: vec![body] },
            redirs: vec![],
            background: false,
            negate: false,
            op: NodeOp::End,
            lineno: line,
        })
    }

    fn parse_function_def(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;
        self.advance(); // function keyword
        let name = self.consume().value;
        // Optional ()
        if self.peek_kind() == &TokKind::LParen {
            self.advance();
            if self.peek_kind() == &TokKind::RParen {
                self.advance();
            }
        }
        self.skip_newlines();
        let body = self.parse_compound()?;
        Some(CmdNode {
            kind: CompoundKind::FuncDef { name, body: vec![body] },
            redirs: vec![],
            background: false,
            negate: false,
            op: NodeOp::End,
            lineno: line,
        })
    }

    fn parse_if(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;
        self.advance(); // if
        self.skip_newlines();
        let cond = self.parse_compound_list_until(&[TokKind::Then]);
        self.skip_consume(TokKind::Then);
        self.skip_newlines();
        let then_part = self.parse_compound_list_until(&[TokKind::Elif, TokKind::Else, TokKind::Fi]);

        let mut elif_parts = Vec::new();
        let mut else_part = None;

        loop {
            match self.peek_kind() {
                TokKind::Elif => {
                    self.advance();
                    self.skip_newlines();
                    let ec = self.parse_compound_list_until(&[TokKind::Then]);
                    self.skip_consume(TokKind::Then);
                    self.skip_newlines();
                    let eb = self.parse_compound_list_until(&[TokKind::Elif, TokKind::Else, TokKind::Fi]);
                    elif_parts.push((ec, eb));
                }
                TokKind::Else => {
                    self.advance();
                    self.skip_newlines();
                    let eb = self.parse_compound_list_until(&[TokKind::Fi]);
                    else_part = Some(eb);
                    break;
                }
                _ => break,
            }
        }
        self.skip_consume(TokKind::Fi);
        let redirs = self.parse_redirs();

        Some(CmdNode {
            kind: CompoundKind::If { cond, then_part, elif_parts, else_part },
            redirs,
            background: false,
            negate: false,
            op: NodeOp::End,
            lineno: line,
        })
    }

    fn parse_while(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;
        self.advance(); // while
        self.skip_newlines();
        let cond = self.parse_compound_list_until(&[TokKind::Do]);
        self.skip_consume(TokKind::Do);
        self.skip_newlines();
        let body = self.parse_compound_list_until(&[TokKind::Done]);
        self.skip_consume(TokKind::Done);
        let redirs = self.parse_redirs();
        Some(CmdNode {
            kind: CompoundKind::While { cond, body },
            redirs,
            background: false,
            negate: false,
            op: NodeOp::End,
            lineno: line,
        })
    }

    fn parse_until(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;
        self.advance(); // until
        self.skip_newlines();
        let cond = self.parse_compound_list_until(&[TokKind::Do]);
        self.skip_consume(TokKind::Do);
        self.skip_newlines();
        let body = self.parse_compound_list_until(&[TokKind::Done]);
        self.skip_consume(TokKind::Done);
        let redirs = self.parse_redirs();
        Some(CmdNode {
            kind: CompoundKind::Until { cond, body },
            redirs,
            background: false,
            negate: false,
            op: NodeOp::End,
            lineno: line,
        })
    }

    fn parse_for(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;
        self.advance(); // for
        let var = self.consume().value;
        self.skip_newlines();

        let words = if self.peek_kind() == &TokKind::In {
            self.advance(); // in
            let mut ws = Vec::new();
            while !matches!(self.peek_kind(), TokKind::Semi | TokKind::Newline | TokKind::Do | TokKind::Eof) {
                ws.push(self.consume());
            }
            ws
        } else {
            vec![] // iterate over positional parameters
        };

        // Skip ; or newline before do
        while matches!(self.peek_kind(), TokKind::Semi | TokKind::Newline) {
            self.advance();
        }
        self.skip_consume(TokKind::Do);
        self.skip_newlines();
        let body = self.parse_compound_list_until(&[TokKind::Done]);
        self.skip_consume(TokKind::Done);
        let redirs = self.parse_redirs();

        Some(CmdNode {
            kind: CompoundKind::For { var, words, body },
            redirs,
            background: false,
            negate: false,
            op: NodeOp::End,
            lineno: line,
        })
    }

    fn parse_case(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;
        self.advance(); // case
        let word = self.consume();
        self.skip_newlines();
        if self.peek_kind() == &TokKind::In {
            self.advance();
        }
        self.skip_newlines();

        let mut arms = Vec::new();
        loop {
            if matches!(self.peek_kind(), TokKind::Esac | TokKind::Eof) {
                break;
            }
            // Skip optional (
            if self.peek_kind() == &TokKind::LParen {
                self.advance();
            }
            let mut patterns = Vec::new();
            loop {
                if matches!(self.peek_kind(), TokKind::RParen | TokKind::Eof) {
                    break;
                }
                let p = self.consume();
                patterns.push(p);
                if self.peek().value == "|" {
                    self.advance(); // |
                } else {
                    break;
                }
            }
            if self.peek_kind() == &TokKind::RParen {
                self.advance();
            }
            self.skip_newlines();
            let body = self.parse_case_body();
            arms.push(CaseArm { patterns, body });
            // Consume ;;
            while matches!(self.peek_kind(), TokKind::Semi) {
                if self.peek().value == ";;" {
                    self.advance();
                    break;
                }
                self.advance();
            }
            self.skip_newlines();
        }
        self.skip_consume(TokKind::Esac);
        let redirs = self.parse_redirs();

        Some(CmdNode {
            kind: CompoundKind::Case { word, arms },
            redirs,
            background: false,
            negate: false,
            op: NodeOp::End,
            lineno: line,
        })
    }

    fn parse_case_body(&mut self) -> Vec<CmdNode> {
        // Parse until ;; or esac
        let mut nodes = Vec::new();
        loop {
            if matches!(self.peek_kind(), TokKind::Esac | TokKind::Eof) {
                break;
            }
            if self.peek_kind() == &TokKind::Semi && self.peek().value == ";;" {
                break;
            }
            if let Some(n) = self.parse_and_or() {
                let mut node = n;
                match self.peek_kind() {
                    TokKind::Semi => {
                        if self.peek().value == ";;" {
                            node.op = NodeOp::Semi;
                            nodes.push(node);
                            break;
                        }
                        self.advance();
                        node.op = NodeOp::Semi;
                    }
                    TokKind::Newline => {
                        self.advance();
                        node.op = NodeOp::Semi;
                        self.skip_newlines();
                    }
                    _ => {
                        node.op = NodeOp::End;
                    }
                }
                nodes.push(node);
            } else {
                break;
            }
        }
        nodes
    }

    fn parse_select(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;
        self.advance(); // select
        let var = self.consume().value;
        self.skip_newlines();
        let words = if self.peek_kind() == &TokKind::In {
            self.advance();
            let mut ws = Vec::new();
            while !matches!(self.peek_kind(), TokKind::Semi | TokKind::Newline | TokKind::Do | TokKind::Eof) {
                ws.push(self.consume());
            }
            ws
        } else {
            vec![]
        };
        while matches!(self.peek_kind(), TokKind::Semi | TokKind::Newline) {
            self.advance();
        }
        self.skip_consume(TokKind::Do);
        self.skip_newlines();
        let body = self.parse_compound_list_until(&[TokKind::Done]);
        self.skip_consume(TokKind::Done);
        let redirs = self.parse_redirs();

        Some(CmdNode {
            kind: CompoundKind::Select { var, words, body },
            redirs,
            background: false,
            negate: false,
            op: NodeOp::End,
            lineno: line,
        })
    }

    fn parse_coproc(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;
        self.advance(); // coproc
        let name = if matches!(self.peek_kind(), TokKind::Word) {
            let n = self.peek().value.clone();
            // Check if next is { - if so, this is the name
            if self.tokens.get(self.pos + 1).map(|t| &t.kind) == Some(&TokKind::LBrace) {
                self.advance();
                n
            } else {
                "COPROC".to_string()
            }
        } else {
            "COPROC".to_string()
        };
        self.skip_newlines();
        let body = self.parse_compound()?;
        Some(CmdNode {
            kind: CompoundKind::Coproc { name, body: vec![body] },
            redirs: vec![],
            background: false,
            negate: false,
            op: NodeOp::End,
            lineno: line,
        })
    }

    fn parse_compound_list_until(&mut self, terminators: &[TokKind]) -> Vec<CmdNode> {
        let mut nodes = Vec::new();
        self.skip_newlines();
        loop {
            for t in terminators {
                if self.peek_kind() == t {
                    return nodes;
                }
            }
            if matches!(self.peek_kind(), TokKind::Eof) {
                break;
            }
            if let Some(node) = self.parse_and_or() {
                let mut node = node;
                match self.peek_kind() {
                    TokKind::Semi => {
                        self.advance();
                        node.op = NodeOp::Semi;
                        self.skip_newlines();
                    }
                    TokKind::Bg => {
                        self.advance();
                        node.background = true;
                        node.op = NodeOp::Bg;
                        self.skip_newlines();
                    }
                    TokKind::Newline => {
                        self.advance();
                        node.op = NodeOp::Semi;
                        self.skip_newlines();
                    }
                    _ => {
                        node.op = NodeOp::End;
                    }
                }
                nodes.push(node);
            } else {
                break;
            }
            for t in terminators {
                if self.peek_kind() == t {
                    return nodes;
                }
            }
        }
        nodes
    }

    fn parse_simple_command(&mut self) -> Option<CmdNode> {
        let line = self.peek().line;
        let mut assigns = Vec::new();
        let mut array_assigns: Vec<(String, Vec<String>)> = Vec::new();
        let mut words = Vec::new();
        let mut redirs = Vec::new();

        loop {
            // Check for redirections
            if let Some(redir) = self.try_parse_redir() {
                redirs.push(redir);
                continue;
            }

            match self.peek_kind() {
                TokKind::Word | TokKind::Assign => {
                    let tok = self.consume();
                    // Check if it's an assignment (VAR=val) and no words yet
                    if words.is_empty() && is_assignment(&tok.value) {
                        let eq = tok.value.find('=').unwrap();
                        let k = tok.value[..eq].to_string();
                        let v = tok.value[eq+1..].to_string();
                        // Check for array literal: NAME=(elem1 elem2 ...)
                        if v.is_empty() && self.peek_kind() == &TokKind::LParen {
                            self.advance(); // consume '('
                            let mut elems = Vec::new();
                            loop {
                                match self.peek_kind() {
                                    TokKind::RParen | TokKind::Eof => {
                                        if self.peek_kind() == &TokKind::RParen {
                                            self.advance();
                                        }
                                        break;
                                    }
                                    _ => {
                                        let elem = self.consume();
                                        elems.push(elem.value);
                                    }
                                }
                            }
                            array_assigns.push((k, elems));
                        } else {
                            assigns.push((k, v));
                        }
                    } else {
                        words.push(tok);
                    }
                }
                // Keyword tokens in argument position are treated as plain words
                // (e.g. `type if`, `echo do`, `echo then`)
                _ if !words.is_empty() && is_keyword_tok(self.peek_kind()) => {
                    let mut tok = self.consume();
                    tok.kind = TokKind::Word;
                    words.push(tok);
                }
                _ => break,
            }
        }

        if assigns.is_empty() && array_assigns.is_empty() && words.is_empty() && redirs.is_empty() {
            return None;
        }

        let cmd = Command {
            words,
            assigns,
            array_assigns,
            redirs: redirs.clone(),
            heredoc_content: None,
            background: false,
            lineno: line,
        };

        Some(CmdNode {
            kind: CompoundKind::Simple(cmd),
            redirs: vec![],
            background: false,
            negate: false,
            op: NodeOp::End,
            lineno: line,
        })
    }

    fn try_parse_redir(&mut self) -> Option<FdRedir> {
        match self.peek_kind() {
            TokKind::RedirIn => {
                self.advance();
                let file = self.consume().value;
                Some(FdRedir {
                    src_fd: 0,
                    dst_fd: -2,
                    file: Some(file),
                    append: false,
                    is_input: true,
                    heredoc_content: None,
                    is_herestr: false,
                    is_procsubst: false,
                    dst_fd_word: None,
                })
            }
            TokKind::RedirOut => {
                self.advance();
                let file = self.consume().value;
                Some(FdRedir {
                    src_fd: 1,
                    dst_fd: -2,
                    file: Some(file),
                    append: false,
                    is_input: false,
                    heredoc_content: None,
                    is_herestr: false,
                    is_procsubst: false,
                    dst_fd_word: None,
                })
            }
            TokKind::RedirAppend => {
                self.advance();
                let file = self.consume().value;
                Some(FdRedir {
                    src_fd: 1,
                    dst_fd: -2,
                    file: Some(file),
                    append: true,
                    is_input: false,
                    heredoc_content: None,
                    is_herestr: false,
                    is_procsubst: false,
                    dst_fd_word: None,
                })
            }
            TokKind::RedirHeredoc => {
                let strip = self.peek().value == "<<-";
                self.advance();
                let delim_tok = self.consume();
                // Check for embedded heredoc content
                let (delim, content) = if delim_tok.value.starts_with("\x00HEREDOC\x00") {
                    let rest = &delim_tok.value["\x00HEREDOC\x00".len()..];
                    let null_pos = rest.find('\x00').unwrap_or(rest.len());
                    let delim = rest[..null_pos].to_string();
                    let content = if null_pos + 1 < rest.len() {
                        rest[null_pos+1..].to_string()
                    } else {
                        String::new()
                    };
                    (delim, content)
                } else {
                    (delim_tok.value.clone(), String::new())
                };
                let _ = (strip, delim);
                Some(FdRedir {
                    src_fd: 0,
                    dst_fd: -2,
                    file: None,
                    append: false,
                    is_input: true,
                    heredoc_content: Some(content),
                    is_herestr: false,
                    is_procsubst: false,
                    dst_fd_word: None,
                })
            }
            TokKind::RedirHerestr => {
                self.advance();
                let word = self.consume().value;
                Some(FdRedir {
                    src_fd: 0,
                    dst_fd: -2,
                    file: Some(word),
                    append: false,
                    is_input: true,
                    heredoc_content: None,
                    is_herestr: true,
                    is_procsubst: false,
                    dst_fd_word: None,
                })
            }
            TokKind::RedirFdOut => {
                let fd: i32 = self.peek().value.parse().unwrap_or(1);
                self.advance();
                let file = self.consume().value;
                Some(FdRedir {
                    src_fd: fd,
                    dst_fd: -2,
                    file: Some(file),
                    append: false,
                    is_input: false,
                    heredoc_content: None,
                    is_herestr: false,
                    is_procsubst: false,
                    dst_fd_word: None,
                })
            }
            TokKind::RedirFdIn => {
                let fd: i32 = self.peek().value.parse().unwrap_or(0);
                self.advance();
                let file = self.consume().value;
                Some(FdRedir {
                    src_fd: fd,
                    dst_fd: -2,
                    file: Some(file),
                    append: false,
                    is_input: true,
                    heredoc_content: None,
                    is_herestr: false,
                    is_procsubst: false,
                    dst_fd_word: None,
                })
            }
            TokKind::RedirFdAppend => {
                let fd: i32 = self.peek().value.parse().unwrap_or(1);
                self.advance();
                let file = self.consume().value;
                Some(FdRedir {
                    src_fd: fd,
                    dst_fd: -2,
                    file: Some(file),
                    append: true,
                    is_input: false,
                    heredoc_content: None,
                    is_herestr: false,
                    is_procsubst: false,
                    dst_fd_word: None,
                })
            }
            TokKind::RedirDupOut => {
                // Value is "N&M" or "N&-"
                let val = self.peek().value.clone();
                self.advance();
                let (src_fd, dst_fd) = parse_dup(&val, false);
                // If dst_fd is set, that's it; otherwise next token is target
                if dst_fd >= 0 || dst_fd == -1 {
                    Some(FdRedir {
                        src_fd,
                        dst_fd,
                        dst_fd_word: None,
                        file: None,
                        append: false,
                        is_input: false,
                        heredoc_content: None,
                        is_herestr: false,
                        is_procsubst: false,
                    })
                } else {
                    // Next token is target fd or - (may be a word needing expansion)
                    let target = self.consume().value;
                    let dst = if target == "-" {
                        -1
                    } else if let Ok(n) = target.parse::<i32>() {
                        n
                    } else {
                        -3 // needs word expansion
                    };
                    Some(FdRedir {
                        src_fd,
                        dst_fd: dst,
                        dst_fd_word: if dst == -3 { Some(target) } else { None },
                        file: None,
                        append: false,
                        is_input: false,
                        heredoc_content: None,
                        is_herestr: false,
                        is_procsubst: false,
                    })
                }
            }
            TokKind::RedirDupIn => {
                let val = self.peek().value.clone();
                self.advance();
                let (src_fd, dst_fd) = parse_dup(&val, true);
                if dst_fd >= 0 || dst_fd == -1 {
                    Some(FdRedir {
                        src_fd,
                        dst_fd,
                        dst_fd_word: None,
                        file: None,
                        append: false,
                        is_input: true,
                        heredoc_content: None,
                        is_herestr: false,
                        is_procsubst: false,
                    })
                } else {
                    let target = self.consume().value;
                    let dst = if target == "-" {
                        -1
                    } else if let Ok(n) = target.parse::<i32>() {
                        n
                    } else {
                        -3
                    };
                    Some(FdRedir {
                        src_fd,
                        dst_fd: dst,
                        dst_fd_word: if dst == -3 { Some(target) } else { None },
                        file: None,
                        append: false,
                        is_input: true,
                        heredoc_content: None,
                        is_herestr: false,
                        is_procsubst: false,
                    })
                }
            }
            _ => None,
        }
    }

    fn parse_redirs(&mut self) -> Vec<FdRedir> {
        let mut redirs = Vec::new();
        while let Some(r) = self.try_parse_redir() {
            redirs.push(r);
        }
        redirs
    }

    fn skip_consume(&mut self, kind: TokKind) {
        if self.peek_kind() == &kind {
            self.advance();
        }
    }
}

fn parse_dup(val: &str, _is_input: bool) -> (i32, i32) {
    // val is like "N&M", "N&-", "1" (just src, no target)
    if let Some(amp_pos) = val.find('&') {
        let src: i32 = val[..amp_pos].parse().unwrap_or(1);
        let dst_str = &val[amp_pos+1..];
        if dst_str == "-" {
            return (src, -1);
        }
        if dst_str.is_empty() {
            return (src, -3); // needs next token
        }
        let dst: i32 = dst_str.parse().unwrap_or(-3);
        (src, dst)
    } else {
        // Just a number - src_fd
        let src: i32 = val.parse().unwrap_or(1);
        (src, -3) // needs next token
    }
}

fn is_keyword_tok(k: &TokKind) -> bool {
    matches!(k,
        TokKind::If | TokKind::Then | TokKind::Else | TokKind::Elif | TokKind::Fi |
        TokKind::While | TokKind::Until | TokKind::Do | TokKind::Done |
        TokKind::For | TokKind::In | TokKind::Case | TokKind::Esac |
        TokKind::Select | TokKind::Function | TokKind::Time | TokKind::Coproc
    )
}

fn is_assignment(s: &str) -> bool {
    // VAR=val: VAR must be valid identifier
    if let Some(eq) = s.find('=') {
        let name = &s[..eq];
        if name.is_empty() { return false; }
        let mut chars = name.chars();
        let first = chars.next().unwrap();
        if !first.is_alphabetic() && first != '_' { return false; }
        chars.all(|c| c.is_alphanumeric() || c == '_')
    } else {
        false
    }
}

pub fn parse(tokens: Vec<Token>) -> Vec<CmdNode> {
    let mut parser = Parser::new(tokens);
    parser.parse_list()
}
