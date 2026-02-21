CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2 \
         -I../../components/bbc_video/include

SRCS   = test_mc6845.c \
         ../../components/bbc_video/mc6845.c

TARGET = test_mc6845

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
