# tests/sn76489/Makefile — build and run sn76489 unit tests on host
#
# Usage:
#   make          — build and run
#   make build    — build only
#   make clean    — remove build artifacts

CC      ?= gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror \
           -I../../components/sn76489/include \
           -lm

SRC     := test_sn76489.c \
           ../../components/sn76489/sn76489.c

BIN     := test_sn76489

.PHONY: all build run clean

all: build run

build: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
