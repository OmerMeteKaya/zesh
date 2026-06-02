CC      = gcc
CFLAGS  = -Wall -Wextra -std=c23 -Iinclude
LDFLAGS = -lsqlite3 -lcurl -lncurses -ldl

SRC  = $(wildcard src/*.c)
OBJ  = $(SRC:.c=.o)
BIN  = zesh

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

plugins/%.so: plugins/%.c
	$(CC) -shared -fPIC $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJ) $(BIN) plugins/*.so