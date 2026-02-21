CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 \
           -DVR_EMU_6502_STATIC \
           -I../../ext/vrEmu6502/src

VR6502_SRC = ../../ext/vrEmu6502/src/vrEmu6502.c

CA65    = ca65
LD65    = ld65

.PHONY: all test clean

all: test_6502_functional test_6502_interrupt 6502_interrupt_test.bin \
     test_6502_decimal   6502_decimal_test.bin

# --- Functional test ---
test_6502_functional: test_6502_functional.c $(VR6502_SRC)
	$(CC) $(CFLAGS) -o $@ $^

# --- Interrupt test binary (assembled from ca65 source) ---
6502_interrupt_test.bin: 6502_interrupt_test.ca65 interrupt_test.cfg
	$(CA65) -t none -o 6502_interrupt_test.o 6502_interrupt_test.ca65
	$(LD65) 6502_interrupt_test.o -o $@ -m 6502_interrupt_test.map -C interrupt_test.cfg
	@rm -f 6502_interrupt_test.o

# --- Interrupt test harness ---
test_6502_interrupt: test_6502_interrupt.c $(VR6502_SRC)
	$(CC) $(CFLAGS) -o $@ $^

# --- Decimal test binary (assembled from ca65 source) ---
6502_decimal_test.bin: 6502_decimal_test.ca65 decimal_test.cfg
	$(CA65) -t none -o 6502_decimal_test.o 6502_decimal_test.ca65
	$(LD65) 6502_decimal_test.o -o $@ -m 6502_decimal_test.map -C decimal_test.cfg
	@rm -f 6502_decimal_test.o

# --- Decimal test harness ---
test_6502_decimal: test_6502_decimal.c $(VR6502_SRC)
	$(CC) $(CFLAGS) -o $@ $^

# --- Run all tests ---
test: test_6502_functional 6502_functional_test.bin \
      test_6502_interrupt   6502_interrupt_test.bin \
      test_6502_decimal     6502_decimal_test.bin
	@echo "=== Functional Test ==="
	./test_6502_functional 6502_functional_test.bin
	@echo ""
	@echo "=== Interrupt Test ==="
	./test_6502_interrupt 6502_interrupt_test.bin
	@echo ""
	@echo "=== Decimal Mode Test ==="
	./test_6502_decimal 6502_decimal_test.bin

clean:
	rm -f test_6502_functional test_6502_interrupt test_6502_decimal \
	      6502_interrupt_test.bin 6502_interrupt_test.o \
	      6502_interrupt_test.map 6502_interrupt_test.lst \
	      6502_decimal_test.bin 6502_decimal_test.o \
	      6502_decimal_test.map 6502_decimal_test.lst
