#!/usr/bin/env python3
"""
Analyze ripgrep profiling data from multiple threads.

This script reads all profile files from the rg_profile directory,
aggregates the statistics, and produces a summary report.

Usage:
    python3 scripts/analyze_profile.py [profile_directory]

Default profile directory is ./rg_profile
"""

import sys
import os
import re
from pathlib import Path
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List


@dataclass
class OpStats:
    """Statistics for a single operation type."""
    count: int = 0
    total_ns: int = 0
    min_ns: float = float('inf')
    max_ns: int = 0

    def add(self, count: int, total_ns: int, min_ns: float, max_ns: int):
        """Add statistics from another source."""
        self.count += count
        self.total_ns += total_ns
        self.min_ns = min(self.min_ns, min_ns)
        self.max_ns = max(self.max_ns, max_ns)

    @property
    def avg_ns(self) -> float:
        """Calculate average duration in nanoseconds."""
        return self.total_ns / self.count if self.count > 0 else 0

    @property
    def total_ms(self) -> float:
        """Total duration in milliseconds."""
        return self.total_ns / 1_000_000

    @property
    def avg_us(self) -> float:
        """Average duration in microseconds."""
        return self.avg_ns / 1_000

    @property
    def min_us(self) -> float:
        """Minimum duration in microseconds."""
        return self.min_ns / 1_000

    @property
    def max_us(self) -> float:
        """Maximum duration in microseconds."""
        return self.max_ns / 1_000


def parse_duration(duration_str: str) -> int:
    """Parse a Rust Duration debug string to nanoseconds.
    
    Examples:
        "1.234567ms" -> 1234567
        "123.456µs" -> 123456
        "1.234s" -> 1234000000
    """
    duration_str = duration_str.strip()
    
    # Handle different units
    if duration_str.endswith('ns'):
        return int(float(duration_str[:-2]))
    elif duration_str.endswith('µs') or duration_str.endswith('us'):
        return int(float(duration_str[:-2]) * 1_000)
    elif duration_str.endswith('ms'):
        return int(float(duration_str[:-2]) * 1_000_000)
    elif duration_str.endswith('s'):
        return int(float(duration_str[:-1]) * 1_000_000_000)
    else:
        # Assume nanoseconds if no unit
        return int(float(duration_str))


def parse_profile_file(filepath: Path) -> Dict[str, OpStats]:
    """Parse a single profile file and return operation statistics."""
    stats = {}
    current_op = None
    
    with open(filepath, 'r') as f:
        for line in f:
            line = line.strip()
            
            # Match operation name
            if line.startswith('Operation:'):
                current_op = line.split(':', 1)[1].strip()
                stats[current_op] = OpStats()
            
            # Match statistics
            elif current_op and ':' in line:
                key, value = line.split(':', 1)
                key = key.strip()
                value = value.strip()
                
                if key == 'Count':
                    stats[current_op].count = int(value)
                elif key == 'Total':
                    stats[current_op].total_ns = parse_duration(value)
                elif key == 'Min':
                    stats[current_op].min_ns = parse_duration(value)
                elif key == 'Max':
                    stats[current_op].max_ns = parse_duration(value)
    
    return stats


def aggregate_profiles(profile_dir: Path):
    """Aggregate statistics from all profile files in the directory."""
    aggregated: Dict[str, OpStats] = {}
    thread_count = 0
    
    for filepath in sorted(profile_dir.glob('ripgrep_profile_thread_*.txt')):
        thread_count += 1
        thread_stats = parse_profile_file(filepath)
        
        for op_name, op_stats in thread_stats.items():
            if op_name not in aggregated:
                aggregated[op_name] = OpStats()
            aggregated[op_name].add(
                op_stats.count,
                op_stats.total_ns,
                op_stats.min_ns,
                op_stats.max_ns
            )
    
    return aggregated, thread_count


def print_summary(stats: Dict[str, OpStats], thread_count: int):
    """Print a formatted summary of the profiling data."""
    print("=" * 80)
    print(f"Ripgrep Filesystem Profiling Summary")
    print(f"Threads analyzed: {thread_count}")
    print("=" * 80)
    print()
    
    # Sort by total time spent
    sorted_ops = sorted(stats.items(), key=lambda x: x[1].total_ms, reverse=True)
    
    # Calculate total time across all operations
    total_time_ms = sum(op.total_ms for op in stats.values())
    
    print(f"{'Operation':<25} {'Count':>10} {'Total (ms)':>12} {'Avg (µs)':>12} {'Min (µs)':>12} {'Max (µs)':>12} {'% Time':>8}")
    print("-" * 80)
    
    for op_name, op_stats in sorted_ops:
        pct = (op_stats.total_ms / total_time_ms * 100) if total_time_ms > 0 else 0
        print(f"{op_name:<25} {op_stats.count:>10} {op_stats.total_ms:>12.2f} "
              f"{op_stats.avg_us:>12.2f} {op_stats.min_us:>12.2f} "
              f"{op_stats.max_us:>12.2f} {pct:>7.1f}%")
    
    print("-" * 80)
    print(f"{'TOTAL':<25} {sum(s.count for s in stats.values()):>10} "
          f"{total_time_ms:>12.2f} {'':>12} {'':>12} {'':>12} {'100.0%':>8}")
    print()
    
    # Print insights
    print("Key Insights:")
    print("-" * 80)
    
    if sorted_ops:
        hottest = sorted_ops[0]
        print(f"• Hottest operation: {hottest[0]} ({hottest[1].total_ms:.2f}ms, {hottest[1].count} calls)")
        
        most_frequent = max(stats.items(), key=lambda x: x[1].count)
        print(f"• Most frequent operation: {most_frequent[0]} ({most_frequent[1].count} calls)")
        
        slowest_avg = max(stats.items(), key=lambda x: x[1].avg_us)
        print(f"• Slowest average: {slowest_avg[0]} ({slowest_avg[1].avg_us:.2f}µs per call)")
    
    print()


def main():
    """Main entry point."""
    profile_dir = Path(sys.argv[1] if len(sys.argv) > 1 else './rg_profile')
    
    if not profile_dir.exists():
        print(f"Error: Profile directory '{profile_dir}' does not exist.", file=sys.stderr)
        print("\nTo generate profile data, run ripgrep with:", file=sys.stderr)
        print("  RG_PROFILE=1 rg <pattern> <path>", file=sys.stderr)
        sys.exit(1)
    
    profile_files = list(profile_dir.glob('ripgrep_profile_thread_*.txt'))
    if not profile_files:
        print(f"Error: No profile files found in '{profile_dir}'.", file=sys.stderr)
        sys.exit(1)
    
    print(f"Analyzing {len(profile_files)} profile file(s) from {profile_dir}...")
    print()
    
    stats, thread_count = aggregate_profiles(profile_dir)
    
    if not stats:
        print("No profiling data found in the files.", file=sys.stderr)
        sys.exit(1)
    
    print_summary(stats, thread_count)


if __name__ == '__main__':
    main()

# Made with Bob
