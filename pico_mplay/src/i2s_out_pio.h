#pragma once

/*
 * Compiled PIO program for i2s_out.pio from ~/devel/pico_mplay (same
 * author, MIT). Zephyr has no pioasm/CMake integration - PIO programs must
 * be embedded as pre-assembled opcode arrays (see RPI_PICO_PIO_DEFINE_PROGRAM
 * below) - so rather than hand-assembling from the ISA reference, these
 * bytes are lifted directly from that project's build/i2s_out.pio.h
 * (pioasm's own output), which is already correct and verified on real
 * hardware, including the BCLK falling/rising-edge fix found there. See
 * i2s_out.pio's own header comment (in the bare-metal repo) for what this
 * program actually does; regenerate this file from there (rebuild
 * pico_mplay, then `cat build/i2s_out.pio.h`) if that program ever changes.
 *
 * wrap_target=0, wrap=7, side-set width=2 (BCLK+WS) - the generic
 * RPI_PICO_PIO_DEFINE_PROGRAM macro captures the first two; side-set width
 * isn't part of it and has to be set manually via sm_config_set_sideset()
 * wherever this program's default config is built (see main.c).
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
