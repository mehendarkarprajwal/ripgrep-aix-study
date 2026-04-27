/*
 * file_type_benchmark.c
 *
 * Benchmark program to measure file_type operation performance
 * Simulates ripgrep's file_type checking by recursively walking
 * a directory tree and calling stat() on each entry.
 *
 * Compile:
 *   gcc -O2 -o file_type_benchmark file_type_benchmark.c
 *
 * Usage:
 *   ./file_type_benchmark <directory_path>
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
    unsigned long stat_calls;
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

/* Get file type using stat() - this is what we're benchmarking */
static int get_file_type(const char *path, stats_t *stats) {
    struct stat st;
    double start, end, elapsed;
    
    start = get_time_us();
    
    /* This is the critical syscall we're measuring */
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "Warning: lstat failed for %s: %s\n", 
                path, strerror(errno));
        return -1;
    }
    
    end = get_time_us();
    elapsed = end - start;
    
    /* Update statistics */
    stats->stat_calls++;
    stats->total_time_us += elapsed;
    
    if (elapsed < stats->min_time_us) {
        stats->min_time_us = elapsed;
    }
    if (elapsed > stats->max_time_us) {
        stats->max_time_us = elapsed;
    }
    
    /* Classify file type */
    if (S_ISDIR(st.st_mode)) {
        stats->total_dirs++;
        return DT_DIR;
    } else if (S_ISREG(st.st_mode)) {
        stats->total_files++;
        return DT_REG;
    } else if (S_ISLNK(st.st_mode)) {
        stats->total_symlinks++;
        return DT_LNK;
    } else {
        stats->total_other++;
        return DT_UNKNOWN;
    }
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
        
        /* Get file type - THIS IS WHAT WE'RE BENCHMARKING */
        file_type = get_file_type(full_path, stats);
        
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
    double avg_time_us = stats->total_time_us / stats->stat_calls;
    
    printf("\n");
    printf("================================================================================\n");
    printf("File Type Benchmark Results\n");
    printf("================================================================================\n");
    printf("\n");
    printf("Entries Processed:\n");
    printf("  Total entries:        %10lu\n", stats->total_entries);
    printf("  Directories:          %10lu\n", stats->total_dirs);
    printf("  Regular files:        %10lu\n", stats->total_files);
    printf("  Symbolic links:       %10lu\n", stats->total_symlinks);
    printf("  Other:                %10lu\n", stats->total_other);
    printf("\n");
    printf("stat() Performance:\n");
    printf("  Total stat() calls:   %10lu\n", stats->stat_calls);
    printf("  Total time:           %10.2f ms\n", stats->total_time_us / 1000.0);
    printf("  Average time:         %10.2f µs\n", avg_time_us);
    printf("  Min time:             %10.2f µs\n", stats->min_time_us);
    printf("  Max time:             %10.2f µs\n", stats->max_time_us);
    printf("\n");
    printf("Overall Performance:\n");
    printf("  Wall clock time:      %10.2f ms\n", total_wall_time_us / 1000.0);
    printf("  stat() overhead:      %10.1f%%\n", 
           (stats->total_time_us / total_wall_time_us) * 100.0);
    printf("  Throughput:           %10.0f entries/sec\n",
           (stats->total_entries * 1000000.0) / total_wall_time_us);
    printf("\n");
    printf("Comparison to ripgrep profile:\n");
    printf("  Expected calls:       ~512,000 (from profile)\n");
    printf("  Actual calls:         %lu\n", stats->stat_calls);
    printf("  Expected avg (Linux): ~0.11 µs\n");
    printf("  Expected avg (AIX):   ~8.79 µs\n");
    printf("  Measured avg:         %.2f µs\n", avg_time_us);
    printf("================================================================================\n");
}

int main(int argc, char *argv[]) {
    stats_t stats = {0};
    double start_time, end_time, total_time;
    
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <directory_path>\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "This program benchmarks file_type operations by recursively\n");
        fprintf(stderr, "walking a directory tree and calling lstat() on each entry.\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Example:\n");
        fprintf(stderr, "  %s /path/to/kernel/source\n", argv[0]);
        return 1;
    }
    
    /* Initialize stats */
    stats.min_time_us = 1e9;  /* Large initial value */
    stats.max_time_us = 0.0;
    
    printf("Starting file_type benchmark on: %s\n", argv[1]);
    printf("This may take a while for large directory trees...\n\n");
    
    start_time = get_time_us();
    walk_directory(argv[1], &stats, 0);
    end_time = get_time_us();
    
    total_time = end_time - start_time;
    
    print_stats(&stats, total_time);
    
    return 0;
}

// Made with Bob
