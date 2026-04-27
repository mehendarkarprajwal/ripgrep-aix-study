# File Type Benchmark Tool

This C program benchmarks the performance of `lstat()` syscalls, which is what ripgrep uses to determine file types during directory traversal.

## Purpose

The profiling data shows that AIX is **82x slower** than Linux at file type checking:
- **Linux**: 0.11µs average per call (55ms total for 512K calls)
- **AIX**: 8.79µs average per call (4.5s total for 512K calls)

This benchmark helps isolate and measure this specific operation.

## Compilation

### On Linux:
```bash
gcc -O2 -o file_type_benchmark file_type_benchmark.c
```

### On AIX:
```bash
# Using GCC
gcc -O2 -o file_type_benchmark file_type_benchmark.c

# Or using IBM XL C
xlc -O2 -o file_type_benchmark file_type_benchmark.c
```

## Usage

```bash
./file_type_benchmark <directory_path>
```

### Example:
```bash
# Test on the same kernel directory used in ripgrep profiling
./file_type_benchmark ~/Downloads/kernel
```

## What It Measures

The program:
1. Recursively walks the directory tree
2. Calls `lstat()` on every file/directory entry
3. Times each `lstat()` call individually
4. Collects statistics:
   - Total number of entries processed
   - Total time spent in `lstat()` calls
   - Average, min, and max time per call
   - Breakdown by file type (files, dirs, symlinks)

## Expected Output

```
================================================================================
File Type Benchmark Results
================================================================================

Entries Processed:
  Total entries:            512677
  Directories:                5130
  Regular files:            507241
  Symbolic links:              306
  Other:                         0

stat() Performance:
  Total stat() calls:       512677
  Total time:                 54.56 ms    (Linux)
  Total time:               4505.39 ms    (AIX)
  Average time:                0.11 µs    (Linux)
  Average time:                8.79 µs    (AIX)
  Min time:                    0.04 µs
  Max time:                  650.94 µs

Overall Performance:
  Wall clock time:            XXX ms
  stat() overhead:            XX%
  Throughput:                 XXXXX entries/sec

Comparison to ripgrep profile:
  Expected calls:       ~512,000 (from profile)
  Actual calls:         512677
  Expected avg (Linux): ~0.11 µs
  Expected avg (AIX):   ~8.79 µs
  Measured avg:         X.XX µs
================================================================================
```

## Interpreting Results

### On Linux (Expected):
- Average time: **~0.1-0.2µs** per `lstat()` call
- Total time: **~50-100ms** for 512K calls
- stat() overhead: **<5%** of wall clock time

### On AIX (Expected):
- Average time: **~8-10µs** per `lstat()` call
- Total time: **~4-5 seconds** for 512K calls
- stat() overhead: **>50%** of wall clock time

### Performance Factors

The `lstat()` performance depends on:
1. **Filesystem type**: JFS2 (AIX) vs ext4/xfs (Linux)
2. **Kernel caching**: How aggressively metadata is cached
3. **Disk I/O**: Whether inodes are in memory or need disk reads
4. **Network filesystem**: NFS adds significant latency
5. **System load**: Other processes competing for I/O

## Troubleshooting

### If the benchmark is too slow:
- Test on a smaller directory first
- The program prints progress every 10,000 entries
- Press Ctrl+C to abort if needed

### If results don't match ripgrep profile:
- Ensure you're testing the same directory
- Check if the filesystem is cached (run twice)
- Verify no other heavy I/O is running

## Optimization Ideas for AIX

Based on benchmark results, consider:

1. **Reduce stat() calls**: Cache file types when possible
2. **Batch operations**: Group metadata queries if filesystem supports it
3. **Filesystem tuning**: Adjust JFS2 mount options for metadata caching
4. **Use different filesystem**: Test on other filesystems (e.g., GPFS)
5. **Kernel parameters**: Tune AIX kernel for better metadata performance

## Related Files

- [`profiler.rs`](crates/ignore/src/profiler.rs): Profiling framework
- [`walk.rs`](crates/ignore/src/walk.rs:336): Where `file_type()` is called
- [`aix_profile.txt`](aix_profile.txt): AIX profiling results
- [`linux_profile.txt`](linux_profile.txt): Linux profiling results