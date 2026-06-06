// Config loading from TOML
// Location: ~/.zesh/config.toml

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ZeshConfig {
    #[serde(default)]
    pub theme: ThemeConfig,
    #[serde(default)]
    pub features: FeaturesConfig,
    #[serde(default)]
    pub history: HistoryConfig,
    #[serde(default)]
    pub completion: CompletionConfig,
    #[serde(default)]
    pub prompt: PromptConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ThemeConfig {
    #[serde(default = "default_keyword_color")]
    pub keyword_color: String,
    #[serde(default = "default_command_ok_color")]
    pub command_ok_color: String,
    #[serde(default = "default_command_err_color")]
    pub command_err_color: String,
    #[serde(default = "default_string_color")]
    pub string_color: String,
    #[serde(default = "default_variable_color")]
    pub variable_color: String,
    #[serde(default = "default_comment_color")]
    pub comment_color: String,
    #[serde(default = "default_operator_color")]
    pub operator_color: String,
    #[serde(default = "default_path_color")]
    pub path_color: String,
    #[serde(default = "default_flag_color")]
    pub flag_color: String,
    #[serde(default = "default_error_color")]
    pub error_color: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FeaturesConfig {
    #[serde(default = "default_true")]
    pub highlighting: bool,
    #[serde(default = "default_true")]
    pub completions: bool,
    #[serde(default = "default_true")]
    pub suggestions: bool,
    #[serde(default = "default_true")]
    pub smart_cd: bool,
    #[serde(default = "default_true")]
    pub auto_env: bool,
    #[serde(default = "default_true")]
    pub history_local: bool,
    #[serde(default = "default_true")]
    pub syntax_errors: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HistoryConfig {
    #[serde(default = "default_history_max")]
    pub max_entries: usize,
    #[serde(default = "default_false")]
    pub isolated_by_dir: bool,
    #[serde(default = "default_true")]
    pub dedup: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CompletionConfig {
    #[serde(default = "default_true")]
    pub fuzzy: bool,
    #[serde(default = "default_true")]
    pub show_icons: bool,
    #[serde(default = "default_true")]
    pub frecency_sort: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PromptConfig {
    #[serde(default = "default_true")]
    pub show_git: bool,
    #[serde(default = "default_true")]
    pub show_time: bool,
    #[serde(default = "default_true")]
    pub show_user: bool,
    #[serde(default = "default_false")]
    pub show_hostname: bool,
    #[serde(default = "default_prompt_style")]
    pub style: String,
}

// Default functions for serde
fn default_keyword_color() -> String {
    "#7aa2f7".to_string()
}
fn default_command_ok_color() -> String {
    "#9ece6a".to_string()
}
fn default_command_err_color() -> String {
    "#f7768e".to_string()
}
fn default_string_color() -> String {
    "#9ece6a".to_string()
}
fn default_variable_color() -> String {
    "#7aa2f7".to_string()
}
fn default_comment_color() -> String {
    "#565f89".to_string()
}
fn default_operator_color() -> String {
    "#89ddff".to_string()
}
fn default_path_color() -> String {
    "#bb9af7".to_string()
}
fn default_flag_color() -> String {
    "#e0af68".to_string()
}
fn default_error_color() -> String {
    "#f7768e".to_string()
}
fn default_true() -> bool {
    true
}
fn default_false() -> bool {
    false
}
fn default_history_max() -> usize {
    10000
}
fn default_prompt_style() -> String {
    "simple".to_string()
}

impl Default for ThemeConfig {
    fn default() -> Self {
        Self {
            keyword_color: default_keyword_color(),
            command_ok_color: default_command_ok_color(),
            command_err_color: default_command_err_color(),
            string_color: default_string_color(),
            variable_color: default_variable_color(),
            comment_color: default_comment_color(),
            operator_color: default_operator_color(),
            path_color: default_path_color(),
            flag_color: default_flag_color(),
            error_color: default_error_color(),
        }
    }
}

impl Default for FeaturesConfig {
    fn default() -> Self {
        Self {
            highlighting: true,
            completions: true,
            suggestions: true,
            smart_cd: true,
            auto_env: true,
            history_local: true,
            syntax_errors: true,
        }
    }
}

impl Default for HistoryConfig {
    fn default() -> Self {
        Self {
            max_entries: 10000,
            isolated_by_dir: false,
            dedup: true,
        }
    }
}

impl Default for CompletionConfig {
    fn default() -> Self {
        Self {
            fuzzy: true,
            show_icons: true,
            frecency_sort: true,
        }
    }
}

impl Default for PromptConfig {
    fn default() -> Self {
        Self {
            show_git: true,
            show_time: true,
            show_user: true,
            show_hostname: false,
            style: "simple".to_string(),
        }
    }
}

impl Default for ZeshConfig {
    fn default() -> Self {
        Self {
            theme: ThemeConfig::default(),
            features: FeaturesConfig::default(),
            history: HistoryConfig::default(),
            completion: CompletionConfig::default(),
            prompt: PromptConfig::default(),
        }
    }
}

fn config_path() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/root".to_string());
    PathBuf::from(home).join(".zesh/config.toml")
}

pub fn config_load() -> ZeshConfig {
    let path = config_path();
    if !path.exists() {
        write_default_config();
        return ZeshConfig::default();
    }
    let content = match std::fs::read_to_string(&path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("zesh: config read error: {}, using defaults", e);
            return ZeshConfig::default();
        }
    };
    match toml::from_str(&content) {
        Ok(cfg) => cfg,
        Err(e) => {
            eprintln!("zesh: config parse error: {}, using defaults", e);
            ZeshConfig::default()
        }
    }
}

pub fn write_default_config() {
    let path = config_path();
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if path.exists() {
        return;
    }
    let default_toml = "# Zesh Configuration\n\
# Location: ~/.zesh/config.toml\n\
# This file is automatically generated on first run with sensible defaults.\n\
# All keys are optional; missing keys or invalid config falls back to defaults.\n\
\n\
[theme]\n\
# Colors are in hex format (#RRGGBB)\n\
keyword_color = \"#7aa2f7\"\n\
command_ok_color = \"#9ece6a\"\n\
command_err_color = \"#f7768e\"\n\
string_color = \"#9ece6a\"\n\
variable_color = \"#7aa2f7\"\n\
comment_color = \"#565f89\"\n\
operator_color = \"#89ddff\"\n\
path_color = \"#bb9af7\"\n\
flag_color = \"#e0af68\"\n\
error_color = \"#f7768e\"\n\
\n\
[features]\n\
# Enable/disable shell features\n\
highlighting = true          # Syntax highlighting\n\
completions = true           # Tab completion\n\
suggestions = true           # Command suggestions\n\
smart_cd = true              # Smart directory changing\n\
auto_env = true              # Auto-load .zesh-env files\n\
history_local = true         # Directory-isolated history\n\
syntax_errors = true         # Highlight syntax errors inline\n\
\n\
[history]\n\
# Command history settings\n\
max_entries = 10000          # Maximum history entries\n\
isolated_by_dir = false      # Keep separate history per directory\n\
dedup = true                 # Remove duplicate consecutive commands\n\
\n\
[completion]\n\
# Tab completion settings\n\
fuzzy = true                 # Fuzzy matching\n\
show_icons = true            # Show file type icons\n\
frecency_sort = true         # Sort by frequency + recency\n\
\n\
[prompt]\n\
# Prompt configuration\n\
show_git = true              # Show git branch in prompt\n\
show_time = true             # Show timestamp\n\
show_user = true             # Show username\n\
show_hostname = false        # Show hostname\n\
style = \"simple\"             # Prompt style: simple, powerline, minimal\n";
    if let Err(e) = std::fs::write(&path, default_toml) {
        eprintln!("zesh: failed to write default config: {}", e);
    }
}
