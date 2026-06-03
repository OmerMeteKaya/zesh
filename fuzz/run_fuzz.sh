#!/usr/bin/env bash
# Run an AFL++ fuzzing session against a Zesh target.
#
# Usage:   ./run_fuzz.sh [lexer|parser|expand|script] [hours]
# Example: ./run_fuzz.sh parser 8
#
# Results land in fuzz/findings/<target>/ (crashes/, hangs/, queue/).
set -eu

cd "$(dirname "$0")"            # operate from the fuzz/ directory

TARGET=${1:-parser}
HOURS=${2:-1}

case "$TARGET" in
  lexer|parser|expand|script) ;;
  *) echo "Unknown target '$TARGET' (use: lexer | parser | expand | script)"; exit 2 ;;
esac

# Accept fractional hours (e.g. 0.5) by computing seconds with awk.
SECONDS_TO_RUN=$(awk "BEGIN{printf \"%d\", $HOURS*3600}")

echo "Fuzzing '$TARGET' for $HOURS hour(s) (${SECONDS_TO_RUN}s)"
echo "Results in: fuzz/findings/$TARGET/"

if ! command -v afl-fuzz >/dev/null 2>&1; then
    cat <<'EOF'
AFL++ not found. Install it with:
  git clone https://github.com/AFLplusplus/AFLplusplus
  cd AFLplusplus && make distrib && sudo make install
(Or on Debian/Ubuntu: sudo apt-get install -y afl++)
EOF
    exit 1
fi

# Build the instrumented target.
make "fuzz-${TARGET}-bin"

mkdir -p "findings/$TARGET"

# AFL++ tuning knobs:
#   -t 5000   per-exec timeout (ms)
#   -m 256    memory limit (MB); sqlite/dlopen need some headroom
#   @@        AFL replaces this with the test-case file path
set -x
timeout "$SECONDS_TO_RUN" \
    afl-fuzz -i "corpus/$TARGET" -o "findings/$TARGET" \
    -t 5000 -m 256 \
    -- "./fuzz_${TARGET}" @@ || true
set +x

echo
echo "Fuzzing complete. Inspect crashes/hangs with:"
echo "  ./triage_crashes.sh $TARGET"
