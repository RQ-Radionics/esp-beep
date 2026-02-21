# tests/m6522/Makefile — build and run m6522 unit tests on host
#
# Usage:
#   make          — build and run
#   make build    — build only
#   make clean    — remove build artifacts

CC      ?= gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror \
           -I../../components/m6522/include

SRC     := test_m6522.c \
           ../../components/m6522/m6522.c

BIN     := test_m6522

.PHONY: all build run clean

all: build run

build: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
