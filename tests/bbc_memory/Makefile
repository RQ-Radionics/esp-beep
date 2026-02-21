CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2 \
         -I../../components/bbc_memory

SRCS   = test_bbc_memory.c \
         ../../components/bbc_memory/bbc_memory.c

TARGET = test_bbc_memory

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
