CARGO        = cargo
RELEASE_BIN  = zesh/target/release/zesh_rs
DEBUG_BIN    = zesh/target/debug/zesh_rs
FUZZ_DIR     = zesh/fuzz
CORPUS_DIR   = fuzz/corpus
FINDINGS_DIR = fuzz/findings/rust

.PHONY: all build debug test install clean \
        fuzz-build fuzz-lexer fuzz-parser fuzz-expand fuzz-script \
        fuzz-differential fuzz-all fuzz-status

# Default: release build
all: build

build:
	$(CARGO) build --release --manifest-path zesh/Cargo.toml

debug:
	$(CARGO) build --manifest-path zesh/Cargo.toml

test: build
	$(RELEASE_BIN) test_parite.sh

test-debug: debug
	$(DEBUG_BIN) test_parite.sh

install: build
	install -m 755 $(RELEASE_BIN) /usr/local/bin/zesh
	@echo "Installed to /usr/local/bin/zesh"

# Run directly without installing
run: build
	$(RELEASE_BIN)

clean:
	$(CARGO) clean --manifest-path zesh/Cargo.toml
	$(CARGO) clean --manifest-path $(FUZZ_DIR)/Cargo.toml || true

# --- Fuzzing ---
fuzz-build:
	cd $(FUZZ_DIR) && $(CARGO) afl build --release

fuzz-lexer: fuzz-build
	mkdir -p $(FINDINGS_DIR)/lexer
	$(CARGO) afl fuzz \
	  -i $(CORPUS_DIR)/lexer \
	  -o $(FINDINGS_DIR)/lexer \
	  -t 5000 -m 256 \
	  -- $(FUZZ_DIR)/target/release/fuzz_lexer @@

fuzz-parser: fuzz-build
	mkdir -p $(FINDINGS_DIR)/parser
	$(CARGO) afl fuzz \
	  -i $(CORPUS_DIR)/parser \
	  -o $(FINDINGS_DIR)/parser \
	  -t 5000 -m 256 \
	  -- $(FUZZ_DIR)/target/release/fuzz_parser @@

fuzz-expand: fuzz-build
	mkdir -p $(FINDINGS_DIR)/expand
	$(CARGO) afl fuzz \
	  -i $(CORPUS_DIR)/expand \
	  -o $(FINDINGS_DIR)/expand \
	  -t 5000 -m 256 \
	  -- $(FUZZ_DIR)/target/release/fuzz_expand @@

fuzz-script: fuzz-build
	mkdir -p $(FINDINGS_DIR)/script
	$(CARGO) afl fuzz \
	  -i $(CORPUS_DIR)/script \
	  -o $(FINDINGS_DIR)/script \
	  -t 3000 -m 512 \
	  -- $(FUZZ_DIR)/target/release/fuzz_script @@

fuzz-differential: fuzz-build
	mkdir -p $(FINDINGS_DIR)/differential
	$(CARGO) afl fuzz \
	  -i $(CORPUS_DIR)/lexer \
	  -o $(FINDINGS_DIR)/differential \
	  -t 5000 -m 256 \
	  -- $(FUZZ_DIR)/target/release/fuzz_differential @@

fuzz-all: fuzz-build
	@echo "Start each target in a separate terminal:"
	@echo "  make fuzz-lexer"
	@echo "  make fuzz-parser"
	@echo "  make fuzz-expand"
	@echo "  make fuzz-script"
	@echo "  make fuzz-differential"

fuzz-status:
	@for t in lexer parser expand script differential; do \
	  echo "=== $$t ==="; \
	  grep -E 'execs_done|crashes_found|stability|bitmap_cvg' \
	    $(FINDINGS_DIR)/$$t/default/fuzzer_stats 2>/dev/null || \
	    echo "not running"; \
	  echo; \
	done
