// Variable/function/array tables (thread-safe via Mutex)

use std::collections::HashMap;
use std::sync::Mutex;

// Variable attributes
pub const ATTR_READONLY: u32 = 0x01;
pub const ATTR_INTEGER:  u32 = 0x02;
pub const ATTR_UPPERCASE:u32 = 0x04;
pub const ATTR_LOWERCASE:u32 = 0x08;
pub const ATTR_EXPORT:   u32 = 0x10;
pub const ATTR_LOCAL:    u32 = 0x20;

#[derive(Debug, Clone)]
pub struct Var {
    pub value: String,
    pub attrs: u32,
}

impl Var {
    pub fn new(value: String) -> Self {
        Var { value, attrs: 0 }
    }
    pub fn with_attrs(value: String, attrs: u32) -> Self {
        Var { value, attrs }
    }
}

// A scope layer — local vars live in their own scope
#[derive(Debug, Clone)]
pub struct Scope {
    pub vars: HashMap<String, Var>,
}

impl Scope {
    pub fn new() -> Self {
        Scope { vars: HashMap::new() }
    }
}

// Shell function definition
#[derive(Debug, Clone)]
pub struct ShellFunction {
    pub name: String,
    pub body: Vec<crate::shell::types::CmdNode>,
    pub defined_at_line: u32,
    pub source_file: String,
}

// Arrays: indexed by integer key
pub type ShellArray = HashMap<usize, String>;

// Global variable store
pub struct VarStore {
    // Stack of scopes; index 0 = global, higher = function-local
    pub scopes: Vec<Scope>,
    pub functions: HashMap<String, ShellFunction>,
    pub arrays: HashMap<String, ShellArray>,
    pub hash_table: HashMap<String, String>,  // command hash cache
}

impl VarStore {
    pub fn new() -> Self {
        let mut global = Scope::new();
        // Initialize shell-specific variables
        global.vars.insert("IFS".to_string(), Var::new(" \t\n".to_string()));
        global.vars.insert("PATH".to_string(), Var::new(
            std::env::var("PATH").unwrap_or_else(|_| "/usr/local/bin:/usr/bin:/bin".to_string())
        ));
        global.vars.insert("PS1".to_string(), Var::new("zesh$ ".to_string()));
        global.vars.insert("PS2".to_string(), Var::new("> ".to_string()));
        global.vars.insert("OPTIND".to_string(), Var::new("1".to_string()));
        global.vars.insert("OPTARG".to_string(), Var::new(String::new()));
        VarStore {
            scopes: vec![global],
            functions: HashMap::new(),
            arrays: HashMap::new(),
            hash_table: HashMap::new(),
        }
    }

    pub fn get(&self, name: &str) -> Option<&Var> {
        // Search from innermost scope outward
        for scope in self.scopes.iter().rev() {
            if let Some(v) = scope.vars.get(name) {
                return Some(v);
            }
        }
        None
    }

    pub fn get_str(&self, name: &str) -> Option<String> {
        self.get(name).map(|v| v.value.clone())
    }

    pub fn set_raw(&mut self, name: &str, value: String, attrs: u32) {
        // Check if var is local in any non-global scope
        let scope_count = self.scopes.len();
        if scope_count > 1 {
            // Check if there's a local binding in current function scope
            for i in (1..scope_count).rev() {
                if let Some(v) = self.scopes[i].vars.get(name) {
                    let existing_attrs = v.attrs;
                    if existing_attrs & ATTR_LOCAL != 0 || attrs & ATTR_LOCAL != 0 {
                        let final_attrs = existing_attrs | attrs;
                        let v = self.scopes[i].vars.get_mut(name).unwrap();
                        v.value = value;
                        v.attrs = final_attrs;
                        return;
                    }
                }
            }
        }
        // Set in global or innermost scope depending on context
        let idx = if scope_count > 1 {
            // Check if global scope has it
            if self.scopes[0].vars.contains_key(name) && attrs & ATTR_LOCAL == 0 {
                0
            } else {
                scope_count - 1
            }
        } else {
            0
        };
        if let Some(existing) = self.scopes[idx].vars.get_mut(name) {
            let merged_attrs = existing.attrs | attrs;
            existing.value = value;
            existing.attrs = merged_attrs;
        } else {
            self.scopes[idx].vars.insert(name.to_string(), Var::with_attrs(value, attrs));
        }
    }

    pub fn set(&mut self, name: &str, value: String) {
        // Apply attribute transformations
        let attrs = self.get(name).map(|v| v.attrs).unwrap_or(0);
        let value = apply_attrs(value, attrs);

        // Check readonly
        if attrs & ATTR_READONLY != 0 {
            eprintln!("zesh: {}: readonly variable", name);
            return;
        }

        // For INTEGER attribute, evaluate arithmetic with variable lookup
        let value = if attrs & ATTR_INTEGER != 0 {
            match crate::shell::expand::eval_arith_expr_with_vars(&value, self) {
                Ok(n) => n.to_string(),
                Err(_) => value,
            }
        } else {
            value
        };

        self.set_raw(name, value, 0);
    }

    pub fn set_with_attrs(&mut self, name: &str, value: String, new_attrs: u32) {
        let existing_attrs = self.get(name).map(|v| v.attrs).unwrap_or(0);
        if existing_attrs & ATTR_READONLY != 0 && new_attrs & ATTR_READONLY == 0 {
            eprintln!("zesh: {}: readonly variable", name);
            return;
        }
        let merged = existing_attrs | new_attrs;
        let value = apply_attrs(value, merged);
        let value = if merged & ATTR_INTEGER != 0 {
            match crate::shell::expand::eval_arith_expr_with_vars(&value, self) {
                Ok(n) => n.to_string(),
                Err(_) => value,
            }
        } else {
            value
        };
        self.set_raw(name, value, new_attrs);
    }

    pub fn unset(&mut self, name: &str) {
        let scope_count = self.scopes.len();
        for i in (0..scope_count).rev() {
            if self.scopes[i].vars.remove(name).is_some() {
                return;
            }
        }
        // Also remove from arrays
        self.arrays.remove(name);
    }

    pub fn push_scope(&mut self) {
        self.scopes.push(Scope::new());
    }

    pub fn pop_scope(&mut self) {
        if self.scopes.len() > 1 {
            self.scopes.pop();
        }
    }

    pub fn set_local(&mut self, name: &str, value: String) {
        let scope_idx = self.scopes.len() - 1;
        let attrs = ATTR_LOCAL;
        let value = if let Some(v) = self.scopes[scope_idx].vars.get(name) {
            let a = v.attrs | attrs;
            apply_attrs(value, a)
        } else {
            value
        };
        self.scopes[scope_idx].vars.insert(
            name.to_string(),
            Var::with_attrs(value, attrs),
        );
    }

    pub fn export_env(&self) -> Vec<(String, String)> {
        let mut result = Vec::new();
        for scope in &self.scopes {
            for (k, v) in &scope.vars {
                if v.attrs & ATTR_EXPORT != 0 {
                    result.push((k.clone(), v.value.clone()));
                }
            }
        }
        result
    }

    pub fn get_array(&self, name: &str) -> Option<&ShellArray> {
        self.arrays.get(name)
    }

    pub fn get_array_mut(&mut self, name: &str) -> &mut ShellArray {
        self.arrays.entry(name.to_string()).or_insert_with(HashMap::new)
    }

    pub fn set_array_elem(&mut self, name: &str, idx: usize, value: String) {
        let arr = self.arrays.entry(name.to_string()).or_insert_with(HashMap::new);
        arr.insert(idx, value);
    }

    pub fn array_len(&self, name: &str) -> usize {
        self.arrays.get(name).map(|a| a.len()).unwrap_or(0)
    }

    // Iterate all vars for export to environment
    pub fn all_exported(&self) -> HashMap<String, String> {
        let mut result = HashMap::new();
        // Go global -> local, so local overrides global
        for scope in &self.scopes {
            for (k, v) in &scope.vars {
                if v.attrs & ATTR_EXPORT != 0 {
                    result.insert(k.clone(), v.value.clone());
                }
            }
        }
        result
    }

    // All vars in current scope and parents (for compgen -v)
    pub fn all_vars(&self) -> HashMap<String, String> {
        let mut result = HashMap::new();
        for scope in &self.scopes {
            for (k, v) in &scope.vars {
                result.insert(k.clone(), v.value.clone());
            }
        }
        result
    }
}

fn apply_attrs(value: String, attrs: u32) -> String {
    if attrs & ATTR_UPPERCASE != 0 {
        value.to_uppercase()
    } else if attrs & ATTR_LOWERCASE != 0 {
        value.to_lowercase()
    } else {
        value
    }
}

// Global var store (used by binary, not the C lib)
use std::sync::OnceLock;
static VARS: OnceLock<Mutex<VarStore>> = OnceLock::new();

pub fn vars() -> std::sync::MutexGuard<'static, VarStore> {
    VARS.get_or_init(|| Mutex::new(VarStore::new()))
        .lock()
        .expect("vars lock poisoned")
}

pub fn vars_init() {
    let _ = VARS.get_or_init(|| Mutex::new(VarStore::new()));
}

// Reset global state between fuzz iterations to prevent state leakage.
// Only meaningful in fuzz builds; the fuzz feature gates non-deterministic
// expansions ($$, $RANDOM, $SECONDS) to fixed values.
pub fn reset_fuzz_state() {
    #[cfg(feature = "fuzz")]
    {
        if let Ok(mut v) = VARS.get_or_init(|| Mutex::new(VarStore::new())).lock() {
            *v = VarStore::new();
        }
    }
}
