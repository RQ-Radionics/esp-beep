# BBC Micro emulator — native macOS host build
#
# Requires: SDL2 (brew install sdl2)
# Build:    make
# Run:      ./bbc-beep [disk.ssd]

CC      := clang
CFLAGS  := -std=c11 -Wall -Wextra -O2 -g
CFLAGS  += $(shell sdl2-config --cflags)
LDFLAGS := $(shell sdl2-config --libs) -framework CoreAudio -framework AudioToolbox

ROOT    := ../..
COMP    := $(ROOT)/components

INCS    := -I$(COMP)/bbc_machine/include \
           -I$(COMP)/bbc_memory \
           -I$(COMP)/bbc_cpu \
           -I$(COMP)/bbc_video/include \
           -I$(COMP)/m6522/include \
           -I$(COMP)/wd1770/include \
           -I$(COMP)/sn76489/include \
           -I$(ROOT)/ext/vrEmu6502/src

SRCS    := main_sdl.c \
           $(COMP)/bbc_machine/bbc_machine.c \
           $(COMP)/bbc_memory/bbc_memory.c \
           $(COMP)/bbc_cpu/bbc_cpu.c \
           $(COMP)/bbc_video/bbc_video.c \
           $(COMP)/bbc_video/bbc_video_ula.c \
           $(COMP)/bbc_video/mc6845.c \
           $(COMP)/bbc_video/saa5050.c \
           $(COMP)/bbc_video/saa5050_font.c \
           $(COMP)/m6522/m6522.c \
           $(COMP)/m6522/bbc_sysvia.c \
           $(COMP)/m6522/bbc_uservia.c \
           $(COMP)/wd1770/wd1770.c \
           $(COMP)/sn76489/sn76489.c \
           $(ROOT)/ext/vrEmu6502/src/vrEmu6502.c

TARGET  := bbc-beep

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(INCS) $^ $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)
