CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 \
           -DVR_EMU_6502_STATIC \
           -I../../ext/vrEmu6502/src

SRCS    = test_6502_functional.c \
          ../../ext/vrEmu6502/src/vrEmu6502.c

TARGET  = test_6502_functional
BIN     = 6502_functional_test.bin

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TARGET) $(BIN)
	./$(TARGET) $(BIN)

clean:
	rm -f $(TARGET)
