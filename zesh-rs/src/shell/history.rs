// Shell history (simple file-based)

use std::collections::VecDeque;

pub struct History {
    pub entries: VecDeque<String>,
    pub max_size: usize,
    pub file: Option<String>,
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
