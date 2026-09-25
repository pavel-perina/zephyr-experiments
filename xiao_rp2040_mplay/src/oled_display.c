#include <zephyr/device.h>
#include <zephyr/drivers/display.h>

#include "oled_display.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_PAGES  (OLED_HEIGHT / 8)

/* SSD1306 GDDRAM layout (see ssd1306_write() in the Zephyr driver): pages
 * of 8 vertical pixels packed per byte, page-major then column-minor -
 * frame[page * OLED_WIDTH + x], bit b of that byte is row (page*8 + b).
 * Static, so it starts zeroed (all-black in this driver's default
 * PIXEL_FORMAT_MONO01, no color-inversion set by the shield overlay) -
 * oled_display_init()'s first write doubles as the boot-time blank.
 */
static uint8_t frame[OLED_WIDTH * OLED_PAGES];
static const struct device *disp_dev;

/* Which page (0..OLED_PAGES-1) the next call pushes - see oled_draw_bars(). */
static int next_page;

static int push_page(int page)
{
	struct display_buffer_descriptor desc = {
		.buf_size = OLED_WIDTH,
		.width = OLED_WIDTH,
		.height = 8,
		.pitch = OLED_WIDTH,
	};

	return display_write(disp_dev, 0, page * 8, &desc, &frame[page * OLED_WIDTH]);
}

int oled_display_init(void)
{
	disp_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(disp_dev)) {
		disp_dev = NULL;
		return -1;
	}

	/* frame[] starts zeroed (static) - blank every page once at boot,
	 * one push_page() call each (cheap enough to not chunk this one -
	 * startup, not the render thread's steady-state budget).
	 */
	for (int page = 0; page < OLED_PAGES; page++) {
		int ret = push_page(page);

		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

/* Bottom-up bar graph, n_bars columns spanning the full 128px width with a
 * 1px gap between bars.
 *
 * A full-frame I2C write at this shield's 400kHz Fast-mode rate is ~23ms -
 * longer than four audio buffer periods - so this only pushes ONE 8-row
 * page (128 bytes, ~2.9ms) per call, safely inside one buffer's ~5.33ms
 * budget, cycling through all OLED_PAGES pages across calls (~24Hz full-
 * screen refresh). The local frame redraw itself (pure bit-twiddling, no
 * I2C) is cheap and happens fully every call, so it's never stale - only
 * the slow hardware push is chunked. Safe to call inline from the render
 * thread, same as vu_neopixel_update() - no separate thread needed.
 */
void oled_draw_bars(const uint8_t *heights, int n_bars, uint8_t max_height)
{
	if (!disp_dev) {
		return;
	}

	for (size_t i = 0; i < sizeof(frame); i++) {
		frame[i] = 0;
	}

	int bar_pitch = OLED_WIDTH / n_bars;
	int bar_width = bar_pitch > 1 ? bar_pitch - 1 : 1;

	for (int b = 0; b < n_bars; b++) {
		uint8_t h = heights[b];

		if (h > max_height) {
			h = max_height;
		}
		if (h > OLED_HEIGHT) {
			h = OLED_HEIGHT;
		}

		int x0 = b * bar_pitch;
		int x_end = x0 + bar_width;

		if (x_end > OLED_WIDTH) {
			x_end = OLED_WIDTH;
		}

		for (int y = OLED_HEIGHT - h; y < OLED_HEIGHT; y++) {
			int page = y >> 3;
			uint8_t bit = 1u << (y & 7);

			for (int x = x0; x < x_end; x++) {
				frame[page * OLED_WIDTH + x] |= bit;
			}
		}
	}

	push_page(next_page);
	next_page = (next_page + 1) % OLED_PAGES;
}
