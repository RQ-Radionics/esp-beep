# tests/wd1770/Makefile — build and run wd1770 unit tests on host
#
# Usage:
#   make          — build and run
#   make build    — build only
#   make clean    — remove build artifacts

CC      ?= gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror \
           -I../../components/wd1770/include

SRC     := test_wd1770.c \
           ../../components/wd1770/wd1770.c

BIN     := test_wd1770

.PHONY: all build run clean

all: build run

build: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
