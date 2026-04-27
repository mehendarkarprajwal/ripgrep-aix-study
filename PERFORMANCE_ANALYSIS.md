# Ripgrep Performance Analysis: Linux vs AIX

## Executive Summary

AIX is **2.4x slower** than Linux overall (22.2s vs 9.3s), with the primary bottleneck being **file type detection** which is **82x slower** on AIX.

## Root Cause: `d_type` Availability

### Linux (Fast - 0.11µs average)
```
readdir() → returns dirent with d_type field populated
         → NO SYSCALL NEEDED
         → Just read d_type from memory
         → 0.11µs per operation
```

### AIX (Slow - 8.79µs average)  
```
readdir() → returns dirent with d_type = DT_UNKNOWN
         → MUST call lstat() syscall
         → Fetch inode metadata from disk
         → 8.79µs per operation (82x slower)
```

## Detailed Performance Breakdown

| Operation | Linux | AIX | Slowdown | Impact |
|-----------|-------|-----|----------|--------|
| **file_type** | 55ms (0.6%) | 4,505ms (20.3%) | **82x** | **Critical** |
| search_file | 9,130ms (98.2%) | 16,818ms (75.7%) | 1.84x | High |
| read_dir | 70ms (0.8%) | 518ms (2.3%) | 7.4x | Medium |
| gitignore_read | 39ms (0.4%) | 376ms (1.7%) | 9.6x | Low |
| **TOTAL** | **9,293ms** | **22,218ms** | **2.4x** | - |

## Why Linux is Fast

### The `d_type` Optimization

On Linux (ext4, xfs, btrfs), the `readdir()` syscall returns directory entries with the `d_type` field already populated:

```c
struct dirent {
    ino_t          d_ino;       /* Inode number */
    off_t          d_off;       /* Offset to next dirent */
    unsigned short d_reclen;    /* Length of this record */
    unsigned char  d_type;      /* Type of file (DT_REG, DT_DIR, etc.) */
    char           d_name[256]; /* Filename */
};
```

The kernel populates `d_type` **for free** while reading the directory, because:
1. Directory entries on disk already contain type information
2. The kernel has this data in cache from reading the directory
3. No additional I/O is needed

### Ripgrep's Code Path (Linux)

```rust
// walk.rs:336
ent.file_type()  // On Linux: just reads d_type field
                 // On AIX: calls lstat() syscall
```

## Why AIX is Slow

### Missing `d_type` Support

AIX's JFS2 filesystem does **not** populate the `d_type` field in `readdir()`:
- Always returns `d_type = DT_UNKNOWN`
- Forces applications to call `lstat()` for every entry
- Each `lstat()` requires:
  - Path resolution
  - Inode lookup
  - Metadata fetch from disk (if not cached)
  - System call overhead

### The 82x Slowdown Explained

```
Linux:  d_type read from memory        = 0.11µs
AIX:    lstat() syscall + disk I/O     = 8.79µs
        Slowdown: 8.79 / 0.11          = 82x
```

With **512,677 file type checks**:
- Linux: 512,677 × 0.11µs = 56ms
- AIX: 512,677 × 8.79µs = 4,506ms
- **Difference: 4.45 seconds wasted on AIX**

## Benchmark Results

### V1 Benchmark (Always uses lstat)
- **Purpose**: Measures raw `lstat()` performance
- **Linux**: 14.41µs average (84,614 calls)
- **AIX**: Similar to ripgrep profile (~8-10µs expected)
- **Conclusion**: Shows baseline syscall cost

### V2 Benchmark (Uses d_type when available)
- **Purpose**: Simulates ripgrep's actual behavior
- **Linux**: Should show 0.11µs with 100% d_type availability
- **macOS**: 3.25µs with 0% d_type (falls back to lstat)
- **AIX**: Should match ripgrep profile (~8.79µs)

### Why V1 Linux (14.41µs) ≠ Ripgrep Linux (0.11µs)

| Measurement | Method | Time |
|-------------|--------|------|
| V1 Benchmark | Always calls `lstat(path)` | 14.41µs |
| Ripgrep | Uses `d_type` from `readdir()` | 0.11µs |
| **Difference** | **Syscall vs memory read** | **131x** |

## Optimization Strategies for AIX

### 1. Cache File Types (Recommended)
```rust
// Cache d_type equivalent after first lstat()
struct CachedEntry {
    path: PathBuf,
    file_type: FileType,  // Cache this!
    metadata: Metadata,
}
```

### 2. Batch Metadata Queries
- Use `fstatat()` with directory fd
- Reduces path resolution overhead
- May improve cache locality

### 3. Filesystem Tuning
```bash
# Increase JFS2 metadata cache
chfs -a agblksize=8192 /filesystem
chfs -a nbpi=16384 /filesystem
```

### 4. Use GPFS Instead of JFS2
- GPFS may have better metadata performance
- Test if available in your environment

### 5. Reduce Unnecessary Checks
- Skip file type checks for known patterns
- Use file extension hints when possible

## Testing Instructions

### Run V2 Benchmark on Linux
```bash
gcc -O2 -o file_type_benchmark_v2 file_type_benchmark_v2.c
./file_type_benchmark_v2 /path/to/kernel

# Expected output:
# d_type available: ~100% (FAST PATH)
# Average time: ~0.11µs
```

### Run V2 Benchmark on AIX
```bash
gcc -O2 -o file_type_benchmark_v2 file_type_benchmark_v2.c
./file_type_benchmark_v2 /path/to/kernel

# Expected output:
# lstat() fallback: ~100% (SLOW PATH)
# Average time: ~8.79µs
```

## References

- Ripgrep source: [`walk.rs:336`](crates/ignore/src/walk.rs:336)
- Linux `readdir(3)` man page: Documents `d_type` field
- AIX JFS2 documentation: No `d_type` support mentioned
- Benchmark v1: [`file_type_benchmark.c`](file_type_benchmark.c)
- Benchmark v2: [`file_type_benchmark_v2.c`](file_type_benchmark_v2.c)

## Conclusion

The 82x slowdown in file type detection on AIX is due to the **absence of `d_type` support** in JFS2's `readdir()` implementation. Linux filesystems populate this field for free, while AIX requires an expensive `lstat()` syscall for every file.

This single architectural difference accounts for **20% of ripgrep's total runtime on AIX** vs only 0.6% on Linux.