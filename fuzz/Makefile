# Fuzzing harness for Zesh — lexer, parser, expand_word, script.
#
#   make            build the AFL++-instrumented targets
#   make asan       build ASan/UBSan reproduction targets (no AFL++ needed)
#   make fuzz-lexer | fuzz-parser | fuzz-expand | fuzz-script   run an AFL++ session
#   make corpus     (re)generate the seed corpus
#   make clean      remove build artifacts
#
# The instrumented targets need AFL++ (afl-clang-fast preferred, afl-gcc as
# a fallback). The *_asan targets build with a normal compiler so you can
# reproduce a crash even on a machine without AFL++ installed.

# Prefer afl-clang-fast, then afl-clang, then afl-gcc; fall back to the name
# so `make` still gives a clear "command not found" if AFL++ is absent.
AFL_CC := $(shell command -v afl-clang-fast 2>/dev/null || \
                  command -v afl-clang     2>/dev/null || \
                  command -v afl-gcc       2>/dev/null || \
                  echo afl-clang-fast)

# Plain compiler for the ASan reproduction builds.
CC ?= cc

UNAME  := $(shell uname)
DL     := $(if $(filter Linux,$(UNAME)),-ldl,)

CFLAGS  := -Wall -std=c17 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
           -I../include -DFUZZ_MODE
LDLIBS  := -lsqlite3 $(DL)

# All shell sources except main.c (each fuzz target provides its own main()),
# plus fuzz_stubs.c which supplies the few symbols main.c would otherwise own.
SRC := $(filter-out ../src/main.c,$(wildcard ../src/*.c)) fuzz_stubs.c

TARGETS      := fuzz_lexer fuzz_parser fuzz_expand fuzz_script
ASAN_TARGETS := fuzz_lexer_asan fuzz_parser_asan fuzz_expand_asan fuzz_script_asan

.PHONY: all asan corpus clean \
        fuzz-lexer fuzz-parser fuzz-expand fuzz-script \
        fuzz-lexer-bin fuzz-parser-bin fuzz-expand-bin fuzz-script-bin

all: $(TARGETS)

# --- AFL++-instrumented targets -------------------------------------------
fuzz_lexer:  fuzz_lexer.c  fuzz_common.h $(SRC)
	$(AFL_CC) $(CFLAGS) fuzz_lexer.c  $(SRC) -o $@ $(LDLIBS)

fuzz_parser: fuzz_parser.c fuzz_common.h $(SRC)
	$(AFL_CC) $(CFLAGS) fuzz_parser.c $(SRC) -o $@ $(LDLIBS)

fuzz_expand: fuzz_expand.c fuzz_common.h $(SRC)
	$(AFL_CC) $(CFLAGS) fuzz_expand.c $(SRC) -o $@ $(LDLIBS)

fuzz_script: fuzz_script.c fuzz_common.h $(SRC)
	$(AFL_CC) $(CFLAGS) fuzz_script.c $(SRC) -o $@ $(LDLIBS)

# Aliases used by run_fuzz.sh
fuzz-lexer-bin:  fuzz_lexer
fuzz-parser-bin: fuzz_parser
fuzz-expand-bin: fuzz_expand
fuzz-script-bin: fuzz_script

# --- ASan/UBSan reproduction targets (no AFL++ required) ------------------
asan: $(ASAN_TARGETS)

fuzz_lexer_asan:  fuzz_lexer.c  fuzz_common.h $(SRC)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined fuzz_lexer.c  $(SRC) -o $@ $(LDLIBS)

fuzz_parser_asan: fuzz_parser.c fuzz_common.h $(SRC)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined fuzz_parser.c $(SRC) -o $@ $(LDLIBS)

fuzz_expand_asan: fuzz_expand.c fuzz_common.h $(SRC)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined fuzz_expand.c $(SRC) -o $@ $(LDLIBS)

fuzz_script_asan: fuzz_script.c fuzz_common.h $(SRC)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined fuzz_script.c $(SRC) -o $@ $(LDLIBS)

# --- Running AFL++ ---------------------------------------------------------
# These run a session directly; run_fuzz.sh wraps them with a time budget.
fuzz-lexer: fuzz_lexer
	mkdir -p findings/lexer
	afl-fuzz -i corpus/lexer -o findings/lexer -t 5000 -m 256 -- ./fuzz_lexer @@

fuzz-parser: fuzz_parser
	mkdir -p findings/parser
	afl-fuzz -i corpus/parser -o findings/parser -t 5000 -m 256 -- ./fuzz_parser @@

fuzz-expand: fuzz_expand
	mkdir -p findings/expand
	afl-fuzz -i corpus/expand -o findings/expand -t 5000 -m 256 -- ./fuzz_expand @@

fuzz-script: fuzz_script
	mkdir -p findings/script
	afl-fuzz -i corpus/script -o findings/script -t 3000 -m 512 -- ./fuzz_script @@

# --- Corpus ----------------------------------------------------------------
corpus:
	./make_corpus.sh

clean:
	rm -f $(TARGETS) $(ASAN_TARGETS)
