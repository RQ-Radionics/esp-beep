CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2 \
         -I../../components/bbc_video/include

SRCS   = test_saa5050.c \
         ../../components/bbc_video/saa5050.c \
         ../../components/bbc_video/saa5050_font.c

TARGET = test_saa5050

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
