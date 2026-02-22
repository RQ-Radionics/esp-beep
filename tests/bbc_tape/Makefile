# tests/bbc_tape/Makefile — build and run bbc_tape ACIA unit tests on host
#
# Usage:
#   make          — build and run
#   make build    — build only
#   make clean    — remove build artifacts

CC      ?= gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror \
           -I../../components/bbc_tape/include
LDFLAGS := -lz

SRC     := test_acia.c \
           ../../components/bbc_tape/bbc_tape.c

BIN     := test_acia

.PHONY: all build run clean

all: build run

build: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
