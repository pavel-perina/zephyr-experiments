#pragma once

/*
 * Compiled PIO program for i2s_out.pio from ~/devel/pico_mplay (same
 * author, MIT). Zephyr has no pioasm/CMake integration - PIO programs must
 * be embedded as pre-assembled opcode arrays (see RPI_PICO_PIO_DEFINE_PROGRAM
 * below) - so rather than hand-assembling from the ISA reference, these
 * bytes are lifted directly from that project's build/i2s_out.pio.h
 * (pioasm's own output), which is already correct and verified on real
 * hardware, including the BCLK falling/rising-edge fix found there.
 * Regenerate this file from there (rebuild pico_mplay, then
 * `cat build/i2s_out.pio.h`) if that program ever changes.
 *
 * wrap_target=0, wrap=7, side-set width=2 (BCLK+WS) - the generic
 * RPI_PICO_PIO_DEFINE_PROGRAM macro captures the first two; side-set width
 * isn't part of it and has to be set manually via sm_config_set_sideset()
 * wherever this program's default config is built (see main.c).
 *
 * Original .pio source, for reference (see i2s_out.pio in the bare-metal
 * repo for the full explanation of the I2S falling/rising-edge timing this
 * implements - only the short version is repeated here):
 *
 *   .program i2s_out
 *   .side_set 2
 *
 *   .define BITS_PER_CHANNEL 16
 *
 *   .wrap_target
 *   left_word:
 *       set x, (BITS_PER_CHANNEL - 2)  side 0b01   ; BCLK high: samples the previous frame's last bit
 *   left_loop:
 *       out pins, 1                    side 0b00   ; new bit onto DATA, BCLK low (setup)
 *       jmp x-- left_loop              side 0b01   ; BCLK high: receiver samples this bit
 *       out pins, 1                    side 0b10   ; last (16th) left bit; WS flips to "right", BCLK low
 *   right_word:
 *       set x, (BITS_PER_CHANNEL - 2)  side 0b11   ; BCLK high: samples left channel's last bit
 *   right_loop:
 *       out pins, 1                    side 0b10   ; new bit onto DATA, BCLK low (setup)
 *       jmp x-- right_loop             side 0b11   ; BCLK high: receiver samples this bit
 *       out pins, 1                    side 0b00   ; last (16th) right bit; WS flips to "left", BCLK low
 *   .wrap
 *
 * Instruction-by-instruction mapping to the opcodes below (same order):
 *
 *   0xe82e  set  x, 14      side 1   (left_word)
 *   0x6001  out  pins, 1    side 0   (left_loop)
 *   0x0841  jmp  x--, 1     side 1   (left_loop, target = instruction 1)
 *   0x7001  out  pins, 1    side 2   (right_word's leading bit, WS -> right)
 *   0xf82e  set  x, 14      side 3   (right_word)
 *   0x7001  out  pins, 1    side 2   (right_loop)
 *   0x1845  jmp  x--, 5     side 3   (right_loop, target = instruction 5)
 *   0x6001  out  pins, 1    side 0   (last right bit, WS -> left, wraps to 0)
 */

#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>

RPI_PICO_PIO_DEFINE_PROGRAM(i2s_out, 0, 7,
	0xe82e,
	0x6001,
	0x0841,
	0x7001,
	0xf82e,
	0x7001,
	0x1845,
	0x6001
);
