// Job table

use std::collections::HashMap;
use std::sync::Mutex;

#[derive(Debug, Clone, PartialEq)]
pub enum JobStatus {
    Running,
    Stopped,
    Done(i32),
}

#[derive(Debug, Clone)]
pub struct Job {
    pub id: usize,
    pub pid: i32,
    pub pgid: i32,
    pub cmd: String,
    pub status: JobStatus,
    pub disowned: bool,
}

pub struct JobTable {
    pub jobs: HashMap<usize, Job>,
    pub next_id: usize,
    pub last_pid: i32,  // $!
}

impl JobTable {
    pub fn new() -> Self {
        JobTable {
            jobs: HashMap::new(),
            next_id: 1,
            last_pid: 0,
        }
    }

    pub fn add(&mut self, pid: i32, cmd: String) -> usize {
        let id = self.next_id;
        self.next_id += 1;
        self.last_pid = pid;
        self.jobs.insert(id, Job {
            id,
            pid,
            pgid: pid,
            cmd,
            status: JobStatus::Running,
            disowned: false,
        });
        id
    }

    pub fn remove(&mut self, pid: i32) {
        self.jobs.retain(|_, j| j.pid != pid);
    }

    pub fn find_by_pid(&self, pid: i32) -> Option<&Job> {
        self.jobs.values().find(|j| j.pid == pid)
    }

    pub fn find_by_pid_mut(&mut self, pid: i32) -> Option<&mut Job> {
        self.jobs.values_mut().find(|j| j.pid == pid)
    }

    pub fn disown(&mut self, pid: i32) {
        if let Some(j) = self.find_by_pid_mut(pid) {
            j.disowned = true;
        }
        // Also remove from table
        self.jobs.retain(|_, j| j.pid != pid);
    }

    pub fn all_pids(&self) -> Vec<i32> {
        self.jobs.values()
            .filter(|j| !j.disowned && j.status == JobStatus::Running)
            .map(|j| j.pid)
            .collect()
    }
}

use std::sync::OnceLock;
static JOBS: OnceLock<Mutex<JobTable>> = OnceLock::new();

pub fn jobs() -> std::sync::MutexGuard<'static, JobTable> {
    JOBS.get_or_init(|| Mutex::new(JobTable::new()))
        .lock()
        .expect("jobs lock poisoned")
}
