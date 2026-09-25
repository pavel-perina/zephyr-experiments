#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <stdint.h>

/* Grabs the SSD1306 (zephyr,display chosen node, from the
 * seeed_xiao_expansion_board shield) and blanks it to all-black.
 *
 * The SSD1306 driver never clears its own GDDRAM on init - it just
 * configures the controller and turns blanking off, so without this the
 * panel shows whatever garbage was already sitting in its RAM (looked
 * like noise/static). Returns 0 on success, negative if the display isn't
 * ready - caller decides whether that's fatal.
 */
int oled_display_init(void);

/* Draws heights[0..n_bars-1] as a bottom-up bar graph and pushes the whole
 * frame over I2C in one call (~23ms at this shield's 400kHz I2C rate) -
 * call from spectrum_process()'s own thread, not the render thread (see
 * main.c and the .c file for why that's safe without page-chunking).
 */
/* Returns display_write()'s result (0 on success, -ENODEV if no display). */
int oled_draw_bars(const uint8_t *heights, int n_bars, uint8_t max_height);

#endif
