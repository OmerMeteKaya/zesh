# Zesh

A modern shell written in Rust.

## Build

    make          # release build → zesh-rs/target/release/zesh_rs
    make debug    # debug build
    make test     # build + run test suite (78 PASS 0 FAIL 2 SKIP)
    make install  # install to /usr/local/bin/zesh

## Configuration

    ~/.zesh/config.toml   — generated on first run with defaults

## Fuzzing

    make fuzz-build       # build AFL++ instrumented targets
    make fuzz-lexer       # fuzz the lexer
    make fuzz-parser      # fuzz the parser
    make fuzz-status      # check all running fuzz sessions

## Validation

✓ make build           → exit 0
✓ make test            → 78 PASS 0 FAIL 2 SKIP
✓ make fuzz-build      → exit 0
✓ archive/c_src/       → original C files present
✓ zesh-rs/src/ffi.rs   → removed

## License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.
