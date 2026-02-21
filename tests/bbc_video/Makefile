CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2 \
         -I../../components/bbc_video/include

SRCS   = test_bbc_video.c \
         ../../components/bbc_video/bbc_video.c \
         ../../components/bbc_video/bbc_video_ula.c \
         ../../components/bbc_video/mc6845.c \
         ../../components/bbc_video/saa5050.c \
         ../../components/bbc_video/saa5050_font.c

TARGET = test_bbc_video

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
