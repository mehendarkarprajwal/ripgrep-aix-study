/*
 * file_type_benchmark_v2.c
 * 
 * Improved benchmark that measures file_type the same way ripgrep does:
 * Using DirEntry.file_type() which reads from readdir() on Linux (d_type field)
 * but may require lstat() on AIX if d_type is not available.
 * 
 * This explains why Linux is so fast (0.11µs) - it's reading cached data
 * from readdir(), not making syscalls!
 * 
 * Compile:
 *   gcc -O2 -o file_type_benchmark_v2 file_type_benchmark_v2.c
 * 
 * Usage:
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

/* Define file type constants if not available (AIX compatibility) */
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_LNK
#define DT_LNK 10
#endif

/* Statistics structure */
typedef struct {
    unsigned long total_entries;
    unsigned long total_dirs;
    unsigned long total_files;
    unsigned long total_symlinks;
    unsigned long total_other;
    unsigned long d_type_available;
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

/* 
 * Get file type from dirent - THIS IS WHAT RIPGREP DOES
 * On Linux: d_type is populated by readdir() - NO SYSCALL NEEDED
 * On AIX: d_type may be DT_UNKNOWN, requiring lstat() fallback
 */
static int get_file_type_from_dirent(const char *path, struct dirent *entry, stats_t *stats) {
    double start, end, elapsed;
    int file_type;
    
    start = get_time_us();
    
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
static void walk_directory(const char *path, stats_t *stats, int depth) {
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
        file_type = get_file_type_from_dirent(full_path, entry, stats);
        
        /* Recursively process directories */
        if (file_type == DT_DIR) {
            walk_directory(full_path, stats, depth + 1);
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
static void print_stats(const stats_t *stats, double total_wall_time_us) {
    double avg_time_us = stats->total_time_us / stats->total_entries;
    double d_type_percent = (stats->d_type_available * 100.0) / stats->total_entries;
    double lstat_percent = (stats->lstat_fallback * 100.0) / stats->total_entries;
    
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
    printf("  d_type available:     %10lu (%.1f%%) - FAST PATH\n", 
           stats->d_type_available, d_type_percent);
    printf("  lstat() fallback:     %10lu (%.1f%%) - SLOW PATH\n", 
           stats->lstat_fallback, lstat_percent);
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
    if (d_type_percent > 90.0) {
        printf("  ✓ Linux: d_type is available - explains 0.11µs average!\n");
        printf("    readdir() populates d_type, no syscall needed.\n");
    } else if (lstat_percent > 90.0) {
        printf("  ✗ AIX: d_type not available - requires lstat() syscalls!\n");
        printf("    This explains the 82x slowdown on AIX.\n");
    } else {
        printf("  ⚠ Mixed: Some entries use d_type, others need lstat().\n");
    }
    printf("================================================================================\n");
}

int main(int argc, char *argv[]) {
    stats_t stats = {0};
    double start_time, end_time, total_time;
    
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <directory_path>\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "This program benchmarks file_type operations the same way ripgrep does:\n");
        fprintf(stderr, "- On Linux: Uses d_type from readdir() (fast, no syscall)\n");
        fprintf(stderr, "- On AIX: Falls back to lstat() if d_type unavailable (slow)\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Example:\n");
        fprintf(stderr, "  %s /path/to/kernel/source\n", argv[0]);
        return 1;
    }
    
    /* Initialize stats */
    stats.min_time_us = 1e9;  /* Large initial value */
    stats.max_time_us = 0.0;
    
    printf("Starting file_type benchmark (v2) on: %s\n", argv[1]);
    printf("This may take a while for large directory trees...\n\n");
    
    start_time = get_time_us();
    walk_directory(argv[1], &stats, 0);
    end_time = get_time_us();
    
    total_time = end_time - start_time;
    
    print_stats(&stats, total_time);
    
    return 0;
}

// Made with Bob
