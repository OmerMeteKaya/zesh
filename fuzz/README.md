# Fuzzing Zesh

Local [AFL++](https://github.com/AFLplusplus/AFLplusplus) harnesses for the
parts of Zesh that consume untrusted text: the **lexer**, the **parser**, and
**`expand_word`** (word/parameter/arithmetic expansion).

Fuzzing is **not** part of CI — it runs locally/long-running, which would blow
past GitHub Actions limits.

## What is fuzzed

| Target          | Entry point                              | Looks for |
|-----------------|------------------------------------------|-----------|
| `fuzz_lexer`    | `lex()` → `tokens_free()`                | OOB reads/writes, UAF in tokenization |
| `fuzz_parser`   | `lex()` → `parse_list()` → `cmdlist_free()` | AST bugs, crashes, **hangs** (5s watchdog) |
| `fuzz_expand`   | `expand_word()`                          | overflows in `${...}`, `$((...))`, ANSI-C quoting |

Each target runs in AFL++ **persistent mode** for speed and also compiles to a
plain ASan/UBSan binary (`*_asan`) used for crash reproduction.

> ### ⚠️ Safety: `fuzz_expand` executes substitutions
> `expand_word()` resolves command/process substitution, so an input like
> `$(rm -rf ~)` **will fork and execute**. The harness clears `PATH` and
> `chdir`s to `/tmp` as a thin guard, but **absolute-path commands are not
> contained**. Run `fuzz_expand` only inside a throwaway **container/VM** or
> as a disposable user. `fuzz_lexer` and `fuzz_parser` never execute anything.

## Install AFL++

```sh
# Debian/Ubuntu
sudo apt-get install -y afl++

# Or from source (recommended, newest version)
git clone https://github.com/AFLplusplus/AFLplusplus
cd AFLplusplus && make distrib && sudo make install
```

This provides `afl-clang-fast` (preferred), `afl-gcc`, and `afl-fuzz`.

## Build the fuzz targets

```sh
cd fuzz
make            # builds fuzz_lexer, fuzz_parser, fuzz_expand (AFL++-instrumented)
make asan       # builds the *_asan reproduction binaries (no AFL++ needed)
```

The Makefile auto-detects `afl-clang-fast`/`afl-gcc`. It compiles every
`src/*.c` except `main.c` (each target supplies its own `main`), and links
`-lsqlite3` (plus `-ldl` on Linux).

To just check that the harnesses *compile* without AFL++ installed:

```sh
make CC=cc asan      # uses the system compiler, no instrumentation
```

## Run a fuzzing session

```sh
cd fuzz
./run_fuzz.sh parser 8      # fuzz the parser for 8 hours
./run_fuzz.sh lexer 1       # fuzz the lexer for 1 hour
./run_fuzz.sh expand 0.5    # 30 minutes — CONTAINER ONLY (see warning above)
```

`run_fuzz.sh` builds the target, then runs `afl-fuzz` with a time budget:

```
afl-fuzz -i corpus/<target> -o findings/<target> -t 5000 -m 256 -- ./fuzz_<target> @@
```

Output goes to `fuzz/findings/<target>/` (`queue/`, `crashes/`, `hangs/`).

You can also drive AFL++ directly via make:

```sh
make fuzz-lexer        # afl-fuzz on the lexer (runs until you Ctrl-C)
```

If AFL++ complains about core-dump patterns or CPU governor, follow its
on-screen hints, e.g.:

```sh
echo core | sudo tee /proc/sys/kernel/core_pattern
```

## Reproduce / triage crashes

```sh
cd fuzz
./triage_crashes.sh parser
```

This rebuilds `fuzz_parser_asan` if needed and replays every
`findings/parser/crashes/id:*` input through it, printing the ASan/UBSan
stack trace. To reproduce a single case by hand:

```sh
./fuzz_parser_asan < findings/parser/crashes/id:000000,sig:11,...
# or, equivalently, by path:
./fuzz_parser_asan findings/parser/crashes/id:000000,sig:11,...
```

## Minimize a corpus

After a long run, shrink the queue to a minimal set that preserves coverage,
and minimize individual crash inputs:

```sh
# corpus minimization (coverage-preserving)
afl-cmin -i findings/parser/queue -o corpus/parser.min -- ./fuzz_parser @@

# test-case minimization (shrink one input)
afl-tmin -i findings/parser/crashes/id:000000,sig:11,... -o min.txt -- ./fuzz_parser @@
```

## Seed corpus

`corpus/lexer/` and `corpus/parser/` hold 20 representative shell snippets
(pipes, here-docs, `if`/`for`/`while`/`case`, functions, arrays, arithmetic,
ANSI-C quoting, an empty file, …). `corpus/expand/` holds 10 expansion seeds
(`${VAR:-default}`, `$((1+2*3))`, `${#VAR}`, `${VAR@U}`, …).

Regenerate it any time with:

```sh
make corpus      # runs ./make_corpus.sh (idempotent)
```
