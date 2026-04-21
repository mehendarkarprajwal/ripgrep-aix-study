#!/bin/bash
# Convenience script to run ripgrep with profiling enabled and analyze results

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PROFILE_DIR="./rg_profile"

# Function to print colored messages
info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if ripgrep is built
if ! command -v ./target/release/rg &> /dev/null && ! command -v ./target/debug/rg &> /dev/null; then
    error "ripgrep binary not found. Please build it first:"
    echo "  cargo build --release"
    exit 1
fi

# Determine which binary to use
if [ -f "./target/release/rg" ]; then
    RG_BIN="./target/release/rg"
    info "Using release build: $RG_BIN"
elif [ -f "./target/debug/rg" ]; then
    RG_BIN="./target/debug/rg"
    warn "Using debug build: $RG_BIN (slower than release)"
else
    error "No ripgrep binary found"
    exit 1
fi

# Check if arguments provided
if [ $# -eq 0 ]; then
    error "Usage: $0 <rg arguments>"
    echo ""
    echo "Examples:"
    echo "  $0 pattern /path/to/search"
    echo "  $0 --files /path/to/search"
    echo "  $0 --threads=1 pattern /path"
    exit 1
fi

# Clean up old profile data
if [ -d "$PROFILE_DIR" ]; then
    info "Cleaning up old profile data..."
    rm -rf "$PROFILE_DIR"
fi

# Run ripgrep with profiling
info "Running ripgrep with profiling enabled..."
echo "Command: RG_PROFILE=1 $RG_BIN $*"
echo ""

RG_PROFILE=1 "$RG_BIN" "$@"
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
    info "ripgrep completed successfully"
elif [ $EXIT_CODE -eq 1 ]; then
    info "ripgrep completed (no matches found)"
else
    warn "ripgrep exited with code $EXIT_CODE"
fi

# Check if profile data was generated
if [ ! -d "$PROFILE_DIR" ]; then
    error "No profile data generated. Make sure RG_PROFILE was set correctly."
    exit 1
fi

PROFILE_COUNT=$(ls -1 "$PROFILE_DIR"/ripgrep_profile_thread_*.txt 2>/dev/null | wc -l)
if [ "$PROFILE_COUNT" -eq 0 ]; then
    error "No profile files found in $PROFILE_DIR"
    exit 1
fi

info "Found $PROFILE_COUNT profile file(s)"
echo ""

# Analyze the results
info "Analyzing profile data..."
echo ""

if command -v python3 &> /dev/null; then
    python3 scripts/analyze_profile.py "$PROFILE_DIR"
elif command -v python &> /dev/null; then
    python scripts/analyze_profile.py "$PROFILE_DIR"
else
    error "Python not found. Please install Python 3 to analyze results."
    info "Profile data is available in: $PROFILE_DIR"
    exit 1
fi

echo ""
info "Profile data saved in: $PROFILE_DIR"
info "To re-analyze: python3 scripts/analyze_profile.py $PROFILE_DIR"

# Made with Bob
