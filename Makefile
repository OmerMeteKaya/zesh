CC      = gcc
CFLAGS  = -Wall -Wextra -std=c23 -Iinclude
LDFLAGS = -lsqlite3 -lcurl -lncurses -ldl

# ---- optional Rust backend ------------------------------------------------
# Build with `make USE_RUST=1` to route every ported module through the
# zesh-rs static library. Individual modules can also be toggled on their own
# for incremental testing, e.g.:
#     make USE_RUST_LEXER=1     # only the lexer in Rust, everything else C
#     make USE_RUST=1           # every ported module in Rust
# Each per-module switch defaults to USE_RUST, so USE_RUST=1 turns them all on.
USE_RUST ?= 0
RUST_LIB  = zesh-rs/target/release/libzesh_rs.a

USE_RUST_PARSER    ?= $(USE_RUST)
USE_RUST_EXPAND    ?= $(USE_RUST)
USE_RUST_LEXER     ?= $(USE_RUST)
USE_RUST_JOBS      ?= $(USE_RUST)
USE_RUST_SIGNALS   ?= $(USE_RUST)
USE_RUST_ALIAS     ?= $(USE_RUST)
USE_RUST_SECURITY  ?= $(USE_RUST)
USE_RUST_EXECUTOR  ?= $(USE_RUST)
USE_RUST_BUILTINS  ?= $(USE_RUST)
USE_RUST_HISTORY   ?= $(USE_RUST)
USE_RUST_CONFIG    ?= $(USE_RUST)
USE_RUST_RC        ?= $(USE_RUST)
USE_RUST_HIGHLIGHT ?= $(USE_RUST)
USE_RUST_COMPLETIONS ?= $(USE_RUST)
USE_RUST_INPUT     ?= $(USE_RUST)

RUST_CFLAGS :=
ifeq ($(USE_RUST_PARSER),1)
    RUST_CFLAGS += -DUSE_RUST_PARSER
endif
ifeq ($(USE_RUST_EXPAND),1)
    RUST_CFLAGS += -DUSE_RUST_EXPAND
endif
ifeq ($(USE_RUST_LEXER),1)
    RUST_CFLAGS += -DUSE_RUST_LEXER
endif
ifeq ($(USE_RUST_JOBS),1)
    RUST_CFLAGS += -DUSE_RUST_JOBS
endif
ifeq ($(USE_RUST_SIGNALS),1)
    RUST_CFLAGS += -DUSE_RUST_SIGNALS
endif
ifeq ($(USE_RUST_ALIAS),1)
    RUST_CFLAGS += -DUSE_RUST_ALIAS
endif
ifeq ($(USE_RUST_SECURITY),1)
    RUST_CFLAGS += -DUSE_RUST_SECURITY
endif
ifeq ($(USE_RUST_EXECUTOR),1)
    RUST_CFLAGS += -DUSE_RUST_EXECUTOR
endif
ifeq ($(USE_RUST_BUILTINS),1)
    RUST_CFLAGS += -DUSE_RUST_BUILTINS
endif
ifeq ($(USE_RUST_HISTORY),1)
    RUST_CFLAGS += -DUSE_RUST_HISTORY
endif
ifeq ($(USE_RUST_CONFIG),1)
    RUST_CFLAGS += -DUSE_RUST_CONFIG
endif
ifeq ($(USE_RUST_RC),1)
    RUST_CFLAGS += -DUSE_RUST_RC
endif
ifeq ($(USE_RUST_HIGHLIGHT),1)
    RUST_CFLAGS += -DUSE_RUST_HIGHLIGHT
endif
ifeq ($(USE_RUST_COMPLETIONS),1)
    RUST_CFLAGS += -DUSE_RUST_COMPLETIONS
endif
ifeq ($(USE_RUST_INPUT),1)
    RUST_CFLAGS += -DUSE_RUST_INPUT
endif

# If any module is routed through Rust, compile with its -D guards and link the
# static library (it must precede the system libs it depends on).
ifneq ($(strip $(RUST_CFLAGS)),)
    CFLAGS      += $(RUST_CFLAGS)
    LDFLAGS     := $(RUST_LIB) $(LDFLAGS) -lpthread -lm
    RUST_PREREQ  = rust-lib
endif

SRC  = $(wildcard src/*.c)
OBJ  = $(SRC:.c=.o)
BIN  = zesh

all: $(BIN)

# Build the Rust static library. The C-facing header (include/zesh_rs.h) is
# committed and kept in sync by hand, so no cbindgen step is required here.
rust-lib:
	cargo build --release --manifest-path zesh-rs/Cargo.toml

$(BIN): $(RUST_PREREQ) $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

plugins/%.so: plugins/%.c
	$(CC) -shared -fPIC $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJ) $(BIN) plugins/*.so
	cargo clean --manifest-path zesh-rs/Cargo.toml || true

.PHONY: all rust-lib clean
