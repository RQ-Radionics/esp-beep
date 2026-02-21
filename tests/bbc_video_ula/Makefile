CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2 \
         -I../../components/bbc_video/include

SRCS   = test_bbc_video_ula.c \
         ../../components/bbc_video/bbc_video_ula.c

TARGET = test_bbc_video_ula

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
