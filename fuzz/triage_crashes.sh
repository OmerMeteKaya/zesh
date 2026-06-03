#!/usr/bin/env bash
# Reproduce and triage crashes found by AFL++.
#
# Usage:   ./triage_crashes.sh [lexer|parser|expand]
#
# For each crashing input it replays the case through the ASan/UBSan build
# (built on demand) so you get a full stack trace.
set -u

cd "$(dirname "$0")"            # operate from the fuzz/ directory

TARGET=${1:-parser}
CRASHES_DIR="findings/$TARGET/crashes"
ASAN_BIN="./fuzz_${TARGET}_asan"

if [ ! -d "$CRASHES_DIR" ]; then
    echo "No crashes directory: $CRASHES_DIR"
    echo "(Run ./run_fuzz.sh $TARGET first.)"
    exit 0
fi

# Build the ASan reproduction binary if needed.
if [ ! -x "$ASAN_BIN" ]; then
    echo "Building $ASAN_BIN ..."
    make "fuzz_${TARGET}_asan"
fi

shopt -s nullglob
crashes=("$CRASHES_DIR"/id:*)
COUNT=${#crashes[@]}
echo "Found $COUNT crash(es) for '$TARGET'"
[ "$COUNT" -eq 0 ] && exit 0

for crash in "${crashes[@]}"; do
    echo "=== Reproducing: $crash ==="
    echo "Input (first 3 lines, hex):"
    xxd "$crash" | head -3
    echo "--- sanitizer output ---"
    # Feed via stdin; the harness reads stdin when given no file argument.
    ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 \
        "$ASAN_BIN" < "$crash" 2>&1 | head -30
    echo "---"
done
