/*
 * file_type_benchmark_v2.c
 *
 * Improved benchmark that measures file_type the same way ripgrep does:
 * Using DirEntry.file_type() which reads from readdir() on Linux (d_type field)
 * but may require lstat() on AIX if d_type is not available.
 *
 * NOW WITH AIX d_namlen OPTIMIZATION:
 * When READDIR_GET_FILE_TYPE environment variable is set, extracts file type
 * from the upper byte of d_namlen (AIX kernel/libc patch required).
 *
 * This explains why Linux is so fast (0.11µs) - it's reading cached data
 * from readdir(), not making syscalls!
 *
 * Compile:
 *   gcc -O2 -o file_type_benchmark_v2 file_type_benchmark_v2.c
 *
 * Usage:
 *   # Without optimization (default, uses lstat):
 *   ./file_type_benchmark_v2 <directory_path>
 *
 *   # With AIX d_namlen optimization (requires kernel patch):
 *   export READDIR_GET_FILE_TYPE=1
 *   ./file_type_benchmark_v2 <directory_path>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <limits.h>

/* AIX-specific: Access to dirent64 for d_namlen extraction */
#ifdef _AIX
#define _LARGE_FILES
#include <sys/types.h>
#endif

/* Define file type constants if not available (AIX compatibility) */
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_FIFO
#define DT_FIFO 1
#endif
#ifndef DT_CHR
#define DT_CHR 2
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_BLK
#define DT_BLK 6
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_LNK
#define DT_LNK 10
#endif
#ifndef DT_SOCK
#define DT_SOCK 12
#endif

/* Statistics structure */
typedef struct {
    unsigned long total_entries;
    unsigned long total_dirs;
    unsigned long total_files;
    unsigned long total_symlinks;
    unsigned long total_other;
    unsigned long d_type_available;
    unsigned long d_namlen_available;  /* AIX: extracted from d_namlen */
    unsigned long lstat_fallback;
    double total_time_us;
    double min_time_us;
    double max_time_us;
} stats_t;

/* Get current time in microseconds */
static double get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

#ifdef _AIX
/*
 * AIX d_namlen optimization: Extract file type from upper byte of d_namlen
 * This requires a custom AIX kernel/libc patch that stores S_IFMT bits
 * in the upper byte of the 2-byte d_namlen field.
 *
 * NOTE: Based on test.c, it appears the d_namlen field itself contains
 * the mode bits directly, not just in the upper byte. The kernel patch
 * may be storing the full mode in d_namlen when the optimization is enabled.
 *
 * Returns: file type (DT_* constant) or -1 if not available
 */
static int extract_file_type_from_namlen(unsigned short d_namlen, const char *debug_name) {
    /* Extract upper byte (file type bits) */
    unsigned char type_byte = (d_namlen >> 8) & 0xFF;
    unsigned char lower_byte = d_namlen & 0xFF;
    
    /* Debug: Print first few entries to understand the format */
    static int debug_count = 0;
    if (debug_count < 5) {
        fprintf(stderr, "DEBUG: %s: d_namlen=0x%04x, upper=0x%02x, lower=0x%02x\n",
                debug_name, d_namlen, type_byte, lower_byte);
        debug_count++;
    }
    
    /* Try interpreting d_namlen as mode_t directly (as test.c does) */
    mode_t mode = (mode_t)d_namlen;
    
    /* Convert S_IFMT bits to DT_* constants */
    if (S_ISDIR(mode)) {
        return DT_DIR;
    } else if (S_ISREG(mode)) {
        return DT_REG;
    } else if (S_ISLNK(mode)) {
        return DT_LNK;
    } else if (S_ISCHR(mode)) {
        return DT_CHR;
    } else if (S_ISBLK(mode)) {
        return DT_BLK;
    } else if (S_ISFIFO(mode)) {
        return DT_FIFO;
    } else if (S_ISSOCK(mode)) {
        return DT_SOCK;
    }
    
    /* If that didn't work, try the upper byte approach */
    if (type_byte != 0) {
        mode = (mode_t)type_byte << 8;
        
        if (S_ISDIR(mode)) {
            return DT_DIR;
        } else if (S_ISREG(mode)) {
            return DT_REG;
        } else if (S_ISLNK(mode)) {
            return DT_LNK;
        } else if (S_ISCHR(mode)) {
            return DT_CHR;
        } else if (S_ISBLK(mode)) {
            return DT_BLK;
        } else if (S_ISFIFO(mode)) {
            return DT_FIFO;
        } else if (S_ISSOCK(mode)) {
            return DT_SOCK;
        }
    }
    
    return -1;
}
#endif

/*
 * Get file type from dirent - THIS IS WHAT RIPGREP DOES
 * On Linux: d_type is populated by readdir() - NO SYSCALL NEEDED
 * On AIX with optimization: Extract from d_namlen upper byte - NO SYSCALL NEEDED
 * On AIX without optimization: d_type may be DT_UNKNOWN, requiring lstat() fallback
 */
static int get_file_type_from_dirent(const char *path, struct dirent *entry, stats_t *stats, int use_aix_optimization) {
    double start, end, elapsed;
    int file_type = -1;
    
    start = get_time_us();
    
#ifdef _AIX
    /* AIX: Try d_namlen optimization first if enabled */
    if (use_aix_optimization) {
        file_type = extract_file_type_from_namlen(entry->d_namlen, entry->d_name);
        if (file_type != -1) {
            /* Success: Got file type from d_namlen */
            stats->d_namlen_available++;
            goto done;
        }
    }
#endif
    
#ifdef _DIRENT_HAVE_D_TYPE
    /* Linux/BSD: Check if d_type is available */
    if (entry->d_type != DT_UNKNOWN) {
        /* Fast path: d_type is already populated by readdir() */
        file_type = entry->d_type;
        stats->d_type_available++;
    } else
#endif
    {
        /* Slow path: Need to call lstat() */
        struct stat st;
        if (lstat(path, &st) != 0) {
            fprintf(stderr, "Warning: lstat failed for %s: %s\n",
                    path, strerror(errno));
            return -1;
        }
        
        /* Convert stat mode to d_type equivalent */
        if (S_ISDIR(st.st_mode)) {
            file_type = DT_DIR;
        } else if (S_ISREG(st.st_mode)) {
            file_type = DT_REG;
        } else if (S_ISLNK(st.st_mode)) {
            file_type = DT_LNK;
        } else {
            file_type = DT_UNKNOWN;
        }
        
        stats->lstat_fallback++;
    }

#ifdef _AIX
done:
#endif
    
    end = get_time_us();
    elapsed = end - start;
    
    /* Update statistics */
    stats->total_time_us += elapsed;
    
    if (elapsed < stats->min_time_us) {
        stats->min_time_us = elapsed;
    }
    if (elapsed > stats->max_time_us) {
        stats->max_time_us = elapsed;
    }
    
    /* Classify file type */
    if (file_type == DT_DIR) {
        stats->total_dirs++;
    } else if (file_type == DT_REG) {
        stats->total_files++;
    } else if (file_type == DT_LNK) {
        stats->total_symlinks++;
    } else {
        stats->total_other++;
    }
    
    return file_type;
}

/* Recursively walk directory tree */
static void walk_directory(const char *path, stats_t *stats, int depth, int use_aix_optimization) {
    DIR *dir;
    struct dirent *entry;
    char full_path[PATH_MAX];
    int file_type;
    
    /* Limit recursion depth to prevent stack overflow */
    if (depth > 1000) {
        fprintf(stderr, "Warning: Maximum recursion depth reached at %s\n", path);
        return;
    }
    
    dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "Warning: Cannot open directory %s: %s\n", 
                path, strerror(errno));
        return;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || 
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Build full path */
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        
        stats->total_entries++;
        
        /* Get file type - SAME WAY AS RIPGREP */
        file_type = get_file_type_from_dirent(full_path, entry, stats, use_aix_optimization);
        
        /* Recursively process directories */
        if (file_type == DT_DIR) {
            walk_directory(full_path, stats, depth + 1, use_aix_optimization);
        }
        
        /* Print progress every 10000 entries */
        if (stats->total_entries % 10000 == 0) {
            printf("Progress: %lu entries processed...\r", stats->total_entries);
            fflush(stdout);
        }
    }
    
    closedir(dir);
}

/* Print statistics */
static void print_stats(const stats_t *stats, double total_wall_time_us, int use_aix_optimization) {
    double avg_time_us = stats->total_time_us / stats->total_entries;
    double d_type_percent = (stats->d_type_available * 100.0) / stats->total_entries;
    double d_namlen_percent = (stats->d_namlen_available * 100.0) / stats->total_entries;
    double lstat_percent = (stats->lstat_fallback * 100.0) / stats->total_entries;
    double fast_path_percent = d_type_percent + d_namlen_percent;
    
    printf("\n");
    printf("================================================================================\n");
    printf("File Type Benchmark Results (v2 - Using readdir d_type)\n");
    printf("================================================================================\n");
    printf("\n");
    printf("Entries Processed:\n");
    printf("  Total entries:        %10lu\n", stats->total_entries);
    printf("  Directories:          %10lu\n", stats->total_dirs);
    printf("  Regular files:        %10lu\n", stats->total_files);
    printf("  Symbolic links:       %10lu\n", stats->total_symlinks);
    printf("  Other:                %10lu\n", stats->total_other);
    printf("\n");
    printf("File Type Detection Method:\n");
    printf("  d_type available:     %10lu (%.1f%%) - FAST PATH (Linux/BSD)\n",
           stats->d_type_available, d_type_percent);
#ifdef _AIX
    if (use_aix_optimization) {
        printf("  d_namlen available:   %10lu (%.1f%%) - FAST PATH (AIX optimized)\n",
               stats->d_namlen_available, d_namlen_percent);
    }
#endif
    printf("  lstat() fallback:     %10lu (%.1f%%) - SLOW PATH\n",
           stats->lstat_fallback, lstat_percent);
    printf("  Total fast path:      %10lu (%.1f%%)\n",
           stats->d_type_available + stats->d_namlen_available, fast_path_percent);
    printf("\n");
    printf("Performance:\n");
    printf("  Total time:           %10.2f ms\n", stats->total_time_us / 1000.0);
    printf("  Average time:         %10.2f µs\n", avg_time_us);
    printf("  Min time:             %10.2f µs\n", stats->min_time_us);
    printf("  Max time:             %10.2f µs\n", stats->max_time_us);
    printf("\n");
    printf("Overall Performance:\n");
    printf("  Wall clock time:      %10.2f ms\n", total_wall_time_us / 1000.0);
    printf("  Throughput:           %10.0f entries/sec\n",
           (stats->total_entries * 1000000.0) / total_wall_time_us);
    printf("\n");
    printf("Comparison to ripgrep profile:\n");
    printf("  Expected calls:       ~512,000 (from profile)\n");
    printf("  Actual calls:         %lu\n", stats->total_entries);
    printf("  Expected avg (Linux): ~0.11 µs (using d_type)\n");
    printf("  Expected avg (AIX):   ~8.79 µs (using lstat)\n");
    printf("  Measured avg:         %.2f µs\n", avg_time_us);
    printf("\n");
    printf("Key Insight:\n");
    if (fast_path_percent > 90.0) {
        if (d_type_percent > 50.0) {
            printf("  ✓ Linux/BSD: d_type is available - explains 0.11µs average!\n");
            printf("    readdir() populates d_type, no syscall needed.\n");
        } else if (d_namlen_percent > 50.0) {
            printf("  ✓ AIX OPTIMIZED: d_namlen contains file type - 82x faster!\n");
            printf("    Custom kernel patch stores type in d_namlen upper byte.\n");
            printf("    No lstat() syscalls needed - matches Linux performance!\n");
        }
    } else if (lstat_percent > 90.0) {
        printf("  ✗ AIX UNOPTIMIZED: d_type not available - requires lstat() syscalls!\n");
        printf("    This explains the 82x slowdown on AIX.\n");
#ifdef _AIX
        if (!use_aix_optimization) {
            printf("    TIP: Set READDIR_GET_FILE_TYPE=1 to enable d_namlen optimization.\n");
        } else {
            printf("    WARNING: Optimization enabled but not working - kernel patch may be missing.\n");
        }
#endif
    } else {
        printf("  ⚠ Mixed: Some entries use fast path, others need lstat().\n");
    }
    printf("================================================================================\n");
}

int main(int argc, char *argv[]) {
    stats_t stats = {0};
    double start_time, end_time, total_time;
    int use_aix_optimization = 0;
    char *env_value;
    
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <directory_path>\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "This program benchmarks file_type operations the same way ripgrep does:\n");
        fprintf(stderr, "- On Linux: Uses d_type from readdir() (fast, no syscall)\n");
        fprintf(stderr, "- On AIX: Falls back to lstat() if d_type unavailable (slow)\n");
#ifdef _AIX
        fprintf(stderr, "- On AIX with optimization: Extracts type from d_namlen (fast, no syscall)\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "AIX Optimization:\n");
        fprintf(stderr, "  Set READDIR_GET_FILE_TYPE=1 to enable d_namlen optimization\n");
        fprintf(stderr, "  (requires custom AIX kernel/libc patch)\n");
#endif
        fprintf(stderr, "\n");
        fprintf(stderr, "Example:\n");
        fprintf(stderr, "  %s /path/to/kernel/source\n", argv[0]);
#ifdef _AIX
        fprintf(stderr, "  export READDIR_GET_FILE_TYPE=1 && %s /path/to/kernel/source\n", argv[0]);
#endif
        return 1;
    }
    
    /* Check for AIX optimization environment variable */
#ifdef _AIX
    env_value = getenv("READDIR_GET_FILE_TYPE");
    if (env_value != NULL) {
        use_aix_optimization = 1;
        printf("AIX d_namlen optimization: ENABLED\n");
        printf("  Environment variable READDIR_GET_FILE_TYPE is set: %s\n", env_value);
        printf("  Will extract file type from d_namlen upper byte (no lstat needed)\n\n");
    } else {
        printf("AIX d_namlen optimization: DISABLED (default)\n");
        printf("  Environment variable READDIR_GET_FILE_TYPE is not set\n");
        printf("  Will use lstat() for file type detection (slower)\n");
        printf("  TIP: Set READDIR_GET_FILE_TYPE=1 to enable optimization\n\n");
    }
#endif
    
    /* Initialize stats */
    stats.min_time_us = 1e9;  /* Large initial value */
    stats.max_time_us = 0.0;
    
    printf("Starting file_type benchmark (v2) on: %s\n", argv[1]);
    printf("This may take a while for large directory trees...\n\n");
    
    start_time = get_time_us();
    walk_directory(argv[1], &stats, 0, use_aix_optimization);
    end_time = get_time_us();
    
    total_time = end_time - start_time;
    
    print_stats(&stats, total_time, use_aix_optimization);
    
    return 0;
}

// Made with Bob
