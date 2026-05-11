//! Test program for AIX d_type optimization
//!
//! This program demonstrates and tests the AIX-specific optimization
//! for extracting file type information from d_namlen without lstat().
//!
//! Usage:
//!   cargo run --example aix_dtype_test /path/to/directory

use std::env;
use std::process;

#[cfg(target_os = "aix")]
use ignore::aix_dirent::{
    AixReadDir, get_file_type_char, get_file_type_string,
};

#[cfg(not(target_os = "aix"))]
fn main() {
    eprintln!("This example is only available on AIX systems.");
    eprintln!("The AIX d_type optimization is platform-specific.");
    process::exit(1);
}

#[cfg(target_os = "aix")]
fn main() {
    let args: Vec<String> = env::args().collect();

    if args.len() != 2 {
        eprintln!("Usage: {} <directory_path>", args[0]);
        eprintln!("Example: {} /tmp", args[0]);
        process::exit(1);
    }

    let dir_path = &args[1];

    println!("AIX d_type Optimization Test");
    println!("============================");
    println!();
    println!("Reading directory: {}", dir_path);
    println!();

    // Open directory using AIX-optimized reader
    let mut dir = match AixReadDir::open(dir_path) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("Error opening directory '{}': {}", dir_path, e);
            process::exit(1);
        }
    };

    println!(
        "{:<40} {:<20} {:<10} {}",
        "Filename", "File Type", "Type Char", "Has d_type"
    );
    println!("{}", "=".repeat(80));

    let mut total_entries = 0;
    let mut entries_with_dtype = 0;
    let mut entries_without_dtype = 0;

    // Read all entries
    loop {
        match dir.read_entry() {
            Ok(Some(entry)) => {
                total_entries += 1;

                let file_name = entry.file_name().to_string_lossy();

                match entry.file_type() {
                    Some(ft) => {
                        entries_with_dtype += 1;

                        // Get file type string and character
                        let type_str = if ft.is_file() {
                            "Regular file"
                        } else if ft.is_dir() {
                            "Directory"
                        } else if ft.is_symlink() {
                            "Symbolic link"
                        } else {
                            "Other"
                        };

                        let type_char = if ft.is_file() {
                            '-'
                        } else if ft.is_dir() {
                            'd'
                        } else if ft.is_symlink() {
                            'l'
                        } else {
                            '?'
                        };

                        println!(
                            "{:<40} {:<20} {:<10} {}",
                            file_name, type_str, type_char, "Yes"
                        );
                    }
                    None => {
                        entries_without_dtype += 1;
                        println!(
                            "{:<40} {:<20} {:<10} {}",
                            file_name, "Unknown", "?", "No (needs lstat)"
                        );
                    }
                }
            }
            Ok(None) => {
                // End of directory
                break;
            }
            Err(e) => {
                eprintln!("\nError reading directory entry: {}", e);
                break;
            }
        }
    }

    println!();
    println!("Summary");
    println!("=======");
    println!("Total entries:              {}", total_entries);
    println!(
        "Entries with d_type:        {} ({:.1}%)",
        entries_with_dtype,
        (entries_with_dtype as f64 / total_entries as f64) * 100.0
    );
    println!(
        "Entries without d_type:     {} ({:.1}%)",
        entries_without_dtype,
        (entries_without_dtype as f64 / total_entries as f64) * 100.0
    );
    println!();

    if entries_with_dtype > 0 {
        println!("✓ SUCCESS: AIX d_type optimization is working!");
        println!(
            "  {} entries had file type extracted from d_namlen",
            entries_with_dtype
        );
        println!("  No lstat() calls needed for these entries");
    } else {
        println!("✗ WARNING: No entries had d_type information");
        println!("  This means:");
        println!("  1. The AIX kernel/libc patch may not be applied, OR");
        println!("  2. The filesystem doesn't support the optimization, OR");
        println!(
            "  3. All entries are returning d_namlen with upper byte = 0"
        );
        println!();
        println!("  All entries will require lstat() calls (slow path)");
    }

    println!();
    println!("Performance Impact:");
    println!("-------------------");
    if entries_with_dtype > 0 {
        let time_saved_us = entries_with_dtype as f64 * (8.79 - 0.11);
        let time_saved_ms = time_saved_us / 1000.0;
        println!("Estimated time saved: {:.2}ms", time_saved_ms);
        println!("  (Assuming 8.79µs for lstat vs 0.11µs for d_type read)");
    } else {
        let time_wasted_us = total_entries as f64 * 8.79;
        let time_wasted_ms = time_wasted_us / 1000.0;
        println!("Estimated time spent in lstat: {:.2}ms", time_wasted_ms);
        println!(
            "  (All {} entries require lstat() at 8.79µs each)",
            total_entries
        );
    }
}

// Made with Bob
