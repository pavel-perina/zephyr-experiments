#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

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

#endif
