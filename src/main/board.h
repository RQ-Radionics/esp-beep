/*
 * board.h — Hardware pin definitions for ESP32-SBC-FabGL (Olimex)
 *
 * All GPIO assignments in one place.  Change only this file when porting
 * to a different board.
 *
 * Confirmed from FabGL source (vgabasecontroller.h, soundgen.h) and the
 * standard FabGL/Olimex ESP32-SBC-FabGL pinout.
 * SD card pins marked VERIFY — confirm against schematic Rev B before use.
 *
 * Reference:
 *   https://github.com/OLIMEX/ESP32-SBC-FabGL
 *   https://github.com/OLIMEX/FabGL
 */

#pragma once

#include "driver/gpio.h"

/* -----------------------------------------------------------------------
 * VGA — 64 colours (2 bits per channel), standard FabGL pinout
 * Confirmed: vgabasecontroller.h begin() default example comment
 * ----------------------------------------------------------------------- */
#define BOARD_VGA_R1    GPIO_NUM_22
#define BOARD_VGA_R0    GPIO_NUM_21
#define BOARD_VGA_G1    GPIO_NUM_19
#define BOARD_VGA_G0    GPIO_NUM_18
#define BOARD_VGA_B1    GPIO_NUM_5
#define BOARD_VGA_B0    GPIO_NUM_4
#define BOARD_VGA_HSYNC GPIO_NUM_23
#define BOARD_VGA_VSYNC GPIO_NUM_15

/* -----------------------------------------------------------------------
 * Audio — ESP32 internal DAC, output to 3.5mm jack on board
 * DAC1 = GPIO25.  No external BCK/WS pins needed.
 * Confirmed: FabGL soundgen.h SoundGenMethod::DAC default GPIO
 * ----------------------------------------------------------------------- */
#define BOARD_AUDIO_DAC GPIO_NUM_25

/* -----------------------------------------------------------------------
 * PS/2 Keyboard — standard FabGL/Olimex assignment
 * Confirmed: FabGL PS2Controller default for ESP32-WROVER boards
 * ----------------------------------------------------------------------- */
#define BOARD_PS2_KBD_CLK  GPIO_NUM_33
#define BOARD_PS2_KBD_DATA GPIO_NUM_32

/* -----------------------------------------------------------------------
 * PS/2 Mouse (not used by BBC emulator but defined for completeness)
 * ----------------------------------------------------------------------- */
#define BOARD_PS2_MSE_CLK  GPIO_NUM_26
#define BOARD_PS2_MSE_DATA GPIO_NUM_27

/* -----------------------------------------------------------------------
 * SD card — SPI bus
 * VERIFY against ESP32-SBC-FabGL schematic Rev B before use.
 * These are the most likely values based on available GPIOs on WROVER-E,
 * but must be confirmed.
 * ----------------------------------------------------------------------- */
#define BOARD_SD_MOSI  GPIO_NUM_13   /* VERIFY */
#define BOARD_SD_MISO  GPIO_NUM_35   /* VERIFY — input only on ESP32 */
#define BOARD_SD_CLK   GPIO_NUM_14   /* VERIFY */
#define BOARD_SD_CS    GPIO_NUM_2    /* VERIFY */

/* -----------------------------------------------------------------------
 * SD card mount point
 * ----------------------------------------------------------------------- */
#define BOARD_SD_MOUNT "/sdcard"
