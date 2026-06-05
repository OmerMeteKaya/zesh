CC      = gcc
CFLAGS  = -Wall -Wextra -std=c23 -Iinclude
LDFLAGS = -lsqlite3 -lcurl -lncurses -ldl

# ---- optional Rust backend (parse_list + parameter expansion) -------------
# Build with `make USE_RUST=1` to link the zesh-rs static library and route
# parse_list()/cmdlist_free()/expand_word() through it. Default (0) is the
# pure-C build, unchanged.
USE_RUST ?= 0
RUST_LIB  = zesh-rs/target/release/libzesh_rs.a

ifeq ($(USE_RUST),1)
    CFLAGS      += -DUSE_RUST_PARSER -DUSE_RUST_EXPAND
    # RUST_LIB must precede the system libs it depends on in link order.
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
