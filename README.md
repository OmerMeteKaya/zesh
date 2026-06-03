# Zesh

![CI](https://github.com/OmerMeteKaya/shell1/actions/workflows/ci.yml/badge.svg)

**Zesh** (Zenith Shell) is a bash-compatible interactive shell written in
modern C (C23, with C11/C17 fallbacks). It implements a lexer, recursive
parser, executor, word/parameter/arithmetic expansion, job control, and a
large set of builtins.

## Building

Zesh needs only **SQLite3** (used for smart-`cd` frecency history) and, on
Linux, **libdl** (for plugins).

```sh
# Debian/Ubuntu
sudo apt-get install -y libsqlite3-dev

# Single-command build (gcc)
gcc -Wall -std=c23 -D_POSIX_C_SOURCE=200809L -Iinclude \
    src/*.c -o zesh -lsqlite3 -ldl
```

Notes:

- **GCC < 14 / Clang < 15** don't accept `-std=c23` — use `-std=c2x` instead.
- **macOS**: drop `-ldl` (`dlopen` is in libc) and let `pkg-config` find the
  keg-only SQLite: ``-I$(brew --prefix sqlite3)/include``.
- **FreeBSD**: drop `-ldl`; SQLite headers/libs live under `/usr/local`.

A `Makefile` is also provided (`make`).

## Running the tests

The parity suite is a shell script executed by Zesh itself:

```sh
./zesh test_parite.sh
```

A passing run ends with:

```
RESULT: 76 PASS  0 FAIL  0 SKIP
```

CI additionally runs the suite under AddressSanitizer + UndefinedBehaviorSanitizer.

## Fuzzing

Local AFL++ harnesses for the lexer, parser, and `expand_word` live in
[`fuzz/`](fuzz/). They build in persistent mode and ship with a seed corpus,
a runner, and a crash-triage script:

```sh
cd fuzz
make                       # build AFL++-instrumented targets
./run_fuzz.sh parser 8     # fuzz the parser for 8 hours
./triage_crashes.sh parser # reproduce crashes under ASan/UBSan
```

See [`fuzz/README.md`](fuzz/README.md) for details (install instructions,
corpus minimization, and an important safety note about `fuzz_expand`).

## Platform support

| Platform     | Status      |
|--------------|-------------|
| Linux x86_64 | ✅          |
| Linux ARM64  | ✅          |
| macOS ARM    | ✅          |
| FreeBSD      | 🔄 testing  |

Each platform is built with both GCC and Clang across the `c11`/`c17`/`c23`
standards in CI (see [`.github/workflows/ci.yml`](.github/workflows/ci.yml)).

## License
This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.
