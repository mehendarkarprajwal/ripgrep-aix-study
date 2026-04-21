//! Profiling framework for tracking filesystem operations per thread.
//!
//! This module provides a thread-local profiler that tracks timing information
//! for filesystem operations. Data is collected in memory and dumped to disk
//! when the program terminates.

use std::{
    cell::RefCell,
    collections::HashMap,
    fs::File,
    io::Write,
    path::Path,
    sync::atomic::{AtomicBool, AtomicUsize, Ordering},
    time::{Duration, Instant},
};

/// Global flag to enable/disable profiling
static PROFILING_ENABLED: AtomicBool = AtomicBool::new(false);

/// Global counter for thread IDs
static THREAD_COUNTER: AtomicUsize = AtomicUsize::new(0);

/// Types of filesystem operations we track
#[derive(Debug, Clone, Copy, Hash, Eq, PartialEq)]
pub enum FsOperation {
    ReadDir,
    Metadata,
    FileType,
    OpenFile,
    ReadFile,
    SymlinkMetadata,
    GitignoreRead,
    IgnoreFileRead,
    DecompressFile,
    SearchFile,
    MmapFile,
}

impl FsOperation {
    fn as_str(&self) -> &'static str {
        match self {
            FsOperation::ReadDir => "read_dir",
            FsOperation::Metadata => "metadata",
            FsOperation::FileType => "file_type",
            FsOperation::OpenFile => "open_file",
            FsOperation::ReadFile => "read_file",
            FsOperation::SymlinkMetadata => "symlink_metadata",
            FsOperation::GitignoreRead => "gitignore_read",
            FsOperation::IgnoreFileRead => "ignore_file_read",
            FsOperation::DecompressFile => "decompress_file",
            FsOperation::SearchFile => "search_file",
            FsOperation::MmapFile => "mmap_file",
        }
    }
}

/// Statistics for a single operation type
#[derive(Debug, Clone)]
struct OpStats {
    count: usize,
    total_duration: Duration,
    min_duration: Duration,
    max_duration: Duration,
}

impl OpStats {
    fn new() -> Self {
        OpStats {
            count: 0,
            total_duration: Duration::ZERO,
            min_duration: Duration::MAX,
            max_duration: Duration::ZERO,
        }
    }

    fn record(&mut self, duration: Duration) {
        self.count += 1;
        self.total_duration += duration;
        self.min_duration = self.min_duration.min(duration);
        self.max_duration = self.max_duration.max(duration);
    }

    fn avg_duration(&self) -> Duration {
        if self.count == 0 {
            Duration::ZERO
        } else {
            self.total_duration / self.count as u32
        }
    }
}

/// Thread-local profiler data
struct ProfilerData {
    thread_id: usize,
    stats: HashMap<FsOperation, OpStats>,
}

impl ProfilerData {
    fn new() -> Self {
        let thread_id = THREAD_COUNTER.fetch_add(1, Ordering::SeqCst);
        ProfilerData {
            thread_id,
            stats: HashMap::new(),
        }
    }

    fn record(&mut self, op: FsOperation, duration: Duration) {
        self.stats
            .entry(op)
            .or_insert_with(OpStats::new)
            .record(duration);
    }

    fn dump_to_file(&self, base_path: &Path) -> std::io::Result<()> {
        let filename = format!("ripgrep_profile_thread_{}.txt", self.thread_id);
        let filepath = base_path.join(filename);
        let mut file = File::create(filepath)?;

        writeln!(file, "Thread ID: {}", self.thread_id)?;
        writeln!(file, "=")?;
        writeln!(file)?;

        for (op, stats) in &self.stats {
            writeln!(file, "Operation: {}", op.as_str())?;
            writeln!(file, "  Count: {}", stats.count)?;
            writeln!(file, "  Total: {:?}", stats.total_duration)?;
            writeln!(file, "  Average: {:?}", stats.avg_duration())?;
            writeln!(file, "  Min: {:?}", stats.min_duration)?;
            writeln!(file, "  Max: {:?}", stats.max_duration)?;
            writeln!(file)?;
        }

        Ok(())
    }
}

thread_local! {
    static PROFILER: RefCell<ProfilerData> = RefCell::new(ProfilerData::new());
}

/// Enable profiling globally
pub fn enable_profiling() {
    PROFILING_ENABLED.store(true, Ordering::SeqCst);
}

/// Check if profiling is enabled
pub fn is_enabled() -> bool {
    PROFILING_ENABLED.load(Ordering::SeqCst)
}

/// Record a filesystem operation timing
pub fn record_operation(op: FsOperation, duration: Duration) {
    if is_enabled() {
        PROFILER.with(|p| {
            p.borrow_mut().record(op, duration);
        });
    }
}

/// Dump profiling data for the current thread to disk
pub fn dump_thread_profile(output_dir: &Path) -> std::io::Result<()> {
    if is_enabled() {
        PROFILER.with(|p| {
            p.borrow().dump_to_file(output_dir)
        })
    } else {
        Ok(())
    }
}

/// RAII guard for timing an operation
pub struct ProfileGuard {
    op: FsOperation,
    start: Instant,
}

impl ProfileGuard {
    pub fn new(op: FsOperation) -> Self {
        ProfileGuard {
            op,
            start: Instant::now(),
        }
    }
}

impl Drop for ProfileGuard {
    fn drop(&mut self) {
        if is_enabled() {
            let duration = self.start.elapsed();
            record_operation(self.op, duration);
        }
    }
}

/// Convenience macro for profiling a block of code
#[macro_export]
macro_rules! profile_fs_op {
    ($op:expr, $code:expr) => {{
        let _guard = $crate::profiler::ProfileGuard::new($op);
        $code
    }};
}

// Made with Bob
