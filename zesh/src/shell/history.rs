// Shell history (simple file-based)

use std::collections::VecDeque;

pub struct History {
    pub entries: VecDeque<String>,
    pub max_size: usize,
    pub file: Option<String>,
}
pub fn history_save() {
    history().save();
}
impl History {
    pub fn new() -> Self {
        History {
            entries: VecDeque::new(),
            max_size: 1000,
            file: None,
        }
    }

    pub fn add(&mut self, entry: String) {
        if !entry.trim().is_empty() {
            self.entries.push_back(entry);
            if self.entries.len() > self.max_size {
                self.entries.pop_front();
            }
        }
    }

    pub fn load(&mut self, path: &str) {
        if let Ok(content) = std::fs::read_to_string(path) {
            for line in content.lines() {
                if !line.is_empty() {
                    self.entries.push_back(line.to_string());
                }
            }
        }
        self.file = Some(path.to_string());
    }

    pub fn save(&self) {
        if let Some(path) = &self.file {
            let content: Vec<&str> = self.entries.iter().map(|s| s.as_str()).collect();
            let _ = std::fs::write(path, content.join("\n") + "\n");
        }
    }
}

use std::sync::Mutex;
use std::sync::OnceLock;

static HISTORY: OnceLock<Mutex<History>> = OnceLock::new();

pub fn history() -> std::sync::MutexGuard<'static, History> {
    HISTORY.get_or_init(|| Mutex::new(History::new()))
        .lock()
        .expect("history lock poisoned")
}

pub fn history_init() {
    let _ = HISTORY.get_or_init(|| {
        let mut h = History::new();
        // Try to load from ~/.zesh_history
        if let Ok(home) = std::env::var("HOME") {
            let hist_path = format!("{}/.zesh_history", home);
            h.load(&hist_path);
        }
        Mutex::new(h)
    });
}

pub fn history_add(entry: String) {
    history().add(entry);
}

/// Get entry at offset from most recent (1 = most recent, 2 = second most recent, ...).
pub fn history_get(offset: usize) -> Option<String> {
    let h = history();
    let len = h.entries.len();
    if offset == 0 || offset > len { return None; }
    Some(h.entries[len - offset].clone())
}

/// Total number of stored history entries.
pub fn history_count() -> usize {
    history().entries.len()
}

/// Most recent entry that starts with `prefix` (single-line only).
pub fn history_search_prefix(prefix: &str) -> Option<String> {
    if prefix.is_empty() { return None; }
    let h = history();
    for entry in h.entries.iter().rev() {
        if entry.starts_with(prefix) {
            return Some(entry.clone());
        }
    }
    None
}

/// Up to `limit` most recent entries containing `query` (case-insensitive), plus their 1-based absolute indices.
pub fn history_search_multi(query: &str, limit: usize) -> (Vec<String>, Vec<usize>) {
    let h = history();
    let total = h.entries.len();
    let mut entries = Vec::new();
    let mut ids = Vec::new();
    let q = query.to_lowercase();
    for (i, entry) in h.entries.iter().enumerate().rev() {
        if entries.len() >= limit { break; }
        if q.is_empty() || entry.to_lowercase().contains(&q) {
            entries.push(entry.clone());
            ids.push(total - i);
        }
    }
    (entries, ids)
}
