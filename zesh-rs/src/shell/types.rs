// Shell types: Token and AST

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TokKind {
    Word,
    Assign,       // VAR=value
    Semi,         // ;
    Newline,      // \n
    Pipe,         // |
    PipeErr,      // |&
    And,          // &&
    Or,           // ||
    Bg,           // &
    LParen,       // (
    RParen,       // )
    LBrace,       // {
    RBrace,       // }
    Bang,         // !
    RedirIn,      // <
    RedirOut,     // >
    RedirAppend,  // >>
    RedirHeredoc, // <<
    RedirHerestr, // <<<
    RedirFdOut,   // N>
    RedirFdIn,    // N<
    RedirFdAppend,// N>>
    RedirDupOut,  // >&  or N>&M
    RedirDupIn,   // <&  or N<&M
    If,
    Then,
    Else,
    Elif,
    Fi,
    While,
    Until,
    Do,
    Done,
    For,
    In,
    Case,
    Esac,
    Select,
    Function,
    Time,
    Coproc,
    Eof,
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: TokKind,
    pub value: String,
    pub quoted: bool,
    pub line: u32,
}

// Redirection in a command
#[derive(Debug, Clone)]
pub struct FdRedir {
    pub src_fd: i32,
    pub dst_fd: i32,   // -1 = close, -2 = use file field, -3 = use dst_fd_word
    pub dst_fd_word: Option<String>,  // word to expand for dst_fd (-3)
    pub file: Option<String>,
    pub append: bool,
    pub is_input: bool,
    pub heredoc_content: Option<String>,
    pub is_herestr: bool,
    pub is_procsubst: bool, // <(cmd)
}

// A single command with its words and redirections
#[derive(Debug, Clone)]
pub struct Command {
    pub words: Vec<Token>,         // argv[0..] (after expansion)
    pub assigns: Vec<(String, String)>, // VAR=val before command
    pub array_assigns: Vec<(String, Vec<String>)>, // ARR=(elem1 elem2...)
    pub redirs: Vec<FdRedir>,
    pub heredoc_content: Option<String>,
    pub background: bool,
    pub lineno: u32,
}

// Compound command kinds
#[derive(Debug, Clone)]
pub enum CompoundKind {
    // Standard simple command
    Simple(Command),
    // { list }
    Brace(Vec<CmdNode>),
    // ( list ) — subshell
    Subshell(Vec<CmdNode>),
    // if then elif else fi
    If {
        cond: Vec<CmdNode>,
        then_part: Vec<CmdNode>,
        elif_parts: Vec<(Vec<CmdNode>, Vec<CmdNode>)>,
        else_part: Option<Vec<CmdNode>>,
    },
    // while do done
    While {
        cond: Vec<CmdNode>,
        body: Vec<CmdNode>,
    },
    // until do done
    Until {
        cond: Vec<CmdNode>,
        body: Vec<CmdNode>,
    },
    // for VAR in list; do body; done
    For {
        var: String,
        words: Vec<Token>,
        body: Vec<CmdNode>,
    },
    // case WORD in patterns esac
    Case {
        word: Token,
        arms: Vec<CaseArm>,
    },
    // select VAR in list; do body; done
    Select {
        var: String,
        words: Vec<Token>,
        body: Vec<CmdNode>,
    },
    // function NAME() { body }
    FuncDef {
        name: String,
        body: Vec<CmdNode>,
    },
    // time compound
    Time(Vec<CmdNode>),
    // pipeline
    Pipeline(Vec<CmdNode>, bool /* stderr? */),
    // coproc NAME { cmd }
    Coproc {
        name: String,
        body: Vec<CmdNode>,
    },
}

#[derive(Debug, Clone)]
pub struct CaseArm {
    pub patterns: Vec<Token>,
    pub body: Vec<CmdNode>,
}

// A node in the command list (connected by &&, ||, ;, &, |)
#[derive(Debug, Clone)]
pub struct CmdNode {
    pub kind: CompoundKind,
    pub redirs: Vec<FdRedir>,
    pub background: bool,
    pub negate: bool,
    // How this node connects to the next
    pub op: NodeOp,
    pub lineno: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NodeOp {
    Semi,       // ;
    And,        // &&
    Or,         // ||
    Pipe,       // |
    PipeErr,    // |&
    Bg,         // &
    End,        // end of list
}
