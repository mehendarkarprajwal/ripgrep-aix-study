# Ripgrep Filesystem Profiling Framework

This document describes how to use the profiling framework to analyze filesystem operation performance in ripgrep, particularly useful for diagnosing performance issues on different platforms like AIX.

## Overview

The profiling framework tracks the following filesystem operations per thread:
- `read_dir` - Directory reading operations (`fs::read_dir`)
- `metadata` - File metadata retrieval (`fs::metadata`)
- `symlink_metadata` - Symlink metadata retrieval (`fs::symlink_metadata`)
- `file_type` - File type detection from directory entries
- `gitignore_read` - Reading and parsing .gitignore files
- `ignore_file_read` - Reading and parsing .ignore files
- `search_file` - Complete file search operation (open + read + search)
- `mmap_file` - Memory mapping files for search
- `decompress_file` - Decompressing files before search

Each operation is timed and statistics are collected including:
- Count of operations
- Total time spent
- Average time per operation
- Minimum time
- Maximum time

## Quick Start

### 1. Enable Profiling

Set the `RG_PROFILE` environment variable before running ripgrep:

```bash
RG_PROFILE=1 rg pattern /path/to/search
```

### 2. View Results

Profile data is written to `./rg_profile/` directory with one file per thread:
- `ripgrep_profile_thread_0.txt`
- `ripgrep_profile_thread_1.txt`
- etc.

### 3. Analyze Results

Use the provided Python script to aggregate and analyze the data:

```bash
python3 scripts/analyze_profile.py
```

Or specify a custom profile directory:

```bash
python3 scripts/analyze_profile.py /path/to/profile/dir
```

## Example Output

```
================================================================================
Ripgrep Filesystem Profiling Summary
Threads analyzed: 4
================================================================================

Operation                      Count   Total (ms)     Avg (µs)     Min (µs)     Max (µs)   % Time
--------------------------------------------------------------------------------
read_dir                       1234      1234.56       1000.45        50.23      5000.12    45.2%
metadata                       5678       987.65        173.91        10.45      2000.34    36.1%
file_type                      5678       456.78         80.45         5.12       500.23    16.7%
symlink_metadata                123        54.32        441.63        20.11      1000.45     2.0%
--------------------------------------------------------------------------------
TOTAL                          12713     2733.31                                            100.0%

Key Insights:
--------------------------------------------------------------------------------
• Hottest operation: read_dir (1234.56ms, 1234 calls)
• Most frequent operation: metadata (5678 calls)
• Slowest average: symlink_metadata (441.63µs per call)
```

## Comparing Performance Across Platforms

### On Linux

```bash
# Run with profiling
RG_PROFILE=1 rg pattern /large/directory

# Analyze
python3 scripts/analyze_profile.py > linux_profile.txt
```

### On AIX

```bash
# Run with profiling
RG_PROFILE=1 rg pattern /large/directory

# Analyze
python3 scripts/analyze_profile.py > aix_profile.txt
```

### Compare Results

```bash
# Side-by-side comparison
diff -y linux_profile.txt aix_profile.txt

# Or use your favorite diff tool
vimdiff linux_profile.txt aix_profile.txt
```

## Interpreting Results

### High `read_dir` Time
- Indicates slow directory listing
- Check filesystem performance
- Consider if directory has many entries

### High `metadata` Time
- Indicates slow `stat()` syscalls
- May indicate filesystem or kernel issues
- Check if metadata is cached properly

### High `file_type` Time
- May indicate missing `d_type` support in `readdir()`
- Requires additional syscalls to determine file type
- Common on older filesystems

### High `symlink_metadata` Time
- Indicates slow `lstat()` syscalls
- Check if many symlinks are present
- May indicate filesystem performance issues

## Advanced Usage

### Profile Specific Scenarios

```bash
# Profile with single thread (isolate threading overhead)
RG_PROFILE=1 rg --threads=1 pattern /path

# Profile with many threads
RG_PROFILE=1 rg --threads=16 pattern /path

# Profile without gitignore (isolate traversal performance)
RG_PROFILE=1 rg --no-ignore pattern /path

# Profile file listing only (no search)
RG_PROFILE=1 rg --files /path > /dev/null
```

### Custom Analysis

The profile files are plain text and can be parsed with any tool:

```bash
# Sum all read_dir counts across threads
grep "Count:" rg_profile/ripgrep_profile_thread_*.txt | \
  grep -A1 "read_dir" | grep "Count:" | \
  awk '{sum+=$2} END {print sum}'

# Find maximum metadata time
grep "Max:" rg_profile/ripgrep_profile_thread_*.txt | \
  grep -A4 "metadata" | grep "Max:" | \
  sort -k2 -h | tail -1
```

## Implementation Details

### Code Locations

- **Profiler module**: `crates/ignore/src/profiler.rs`
- **Instrumentation**: `crates/ignore/src/walk.rs`
- **Main integration**: `crates/core/main.rs`
- **Analysis script**: `scripts/analyze_profile.py`

### How It Works

1. **Thread-local storage**: Each thread maintains its own statistics
2. **RAII guards**: `ProfileGuard` automatically times operations via Drop
3. **Zero overhead when disabled**: No performance impact when `RG_PROFILE` is not set
4. **Automatic dumping**: Profile data is written when threads exit

### Adding New Operations

To profile additional operations, add them to `FsOperation` enum in `profiler.rs`:

```rust
pub enum FsOperation {
    ReadDir,
    Metadata,
    // ... existing operations ...
    YourNewOperation,  // Add here
}
```

Then instrument the code:

```rust
let _guard = ProfileGuard::new(FsOperation::YourNewOperation);
// ... code to profile ...
```

## Troubleshooting

### No Profile Files Generated

- Ensure `RG_PROFILE` environment variable is set
- Check that `./rg_profile/` directory is writable
- Verify ripgrep completed successfully

### Missing Operations

- Some operations may not be called depending on search parameters
- Use `--files` to profile traversal without search
- Use `--no-ignore` to skip gitignore processing

### Inaccurate Timings

- Ensure system is not under heavy load
- Run multiple times and average results
- Consider using `nice` or `taskset` for consistent CPU allocation

## Performance Tips

Based on profiling results, you can:

1. **Reduce `read_dir` calls**: Use `--max-depth` to limit recursion
2. **Reduce `metadata` calls**: Use `--type` filters to skip unnecessary checks
3. **Skip symlinks**: Use `--no-follow` if symlinks aren't needed
4. **Disable gitignore**: Use `--no-ignore` if not needed

## Contributing

To extend the profiling framework:

1. Add new operation types to `FsOperation` enum
2. Instrument code with `ProfileGuard`
3. Update analysis script if needed
4. Document new metrics in this file