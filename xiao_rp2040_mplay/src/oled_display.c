#include <errno.h>
#include <string.h>
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

static int push_frame(void)
{
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(frame),
		.width = OLED_WIDTH,
		.height = OLED_HEIGHT,
		.pitch = OLED_WIDTH,
	};

	return display_write(disp_dev, 0, 0, &desc, frame);
}

int oled_display_init(void)
{
	disp_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(disp_dev)) {
		disp_dev = NULL;
		return -1;
	}

	/* frame[] starts zeroed (static) - this doubles as the boot-time blank. */
	return push_frame();
}

/* Bottom-up bar graph, n_bars columns spanning the full 128px width with a
 * 1px gap between bars.
 *
 * A full-frame I2C write at this shield's 400kHz Fast-mode rate is ~23ms.
 * That used to be a problem when this ran inline in the render thread
 * (longer than four audio buffer periods, hence the page-chunking this
 * function used to do) - now that spectrum_process()/oled_draw_bars() run
 * in their own lower-priority thread (see main.c), a single blocking call
 * here can't delay audio: Zephyr's preemptive scheduler always lets the
 * render thread win whenever it has work, regardless of how long this
 * thread blocks. So this pushes the whole frame in one call again.
 */

static void set_pixel_row(int y, int x0, int x_end)
{
	int page = y >> 3;
	uint8_t bit = 1u << (y & 7);

	for (int x = x0; x < x_end; x++) {
		frame[page * OLED_WIDTH + x] |= bit;
	}
}

int oled_draw_trace(const uint8_t *ys, int n)
{
	if (!disp_dev) {
		return -ENODEV;
	}

	memset(frame, 0, sizeof(frame));

	/* Dotted centre line as the zero reference. */
	for (int x = 0; x < OLED_WIDTH; x += 4) {
		set_pixel_row(OLED_HEIGHT / 2, x, x + 1);
	}

	if (n > OLED_WIDTH) {
		n = OLED_WIDTH;
	}
	for (int x = 0; x < n; x++) {
		/* Join to the previous column with a vertical span, so steep
		 * edges stay a continuous line instead of scattered dots.
		 */
		int y0 = ys[x];
		int y1 = (x > 0) ? ys[x - 1] : y0;

		if (y0 > y1) {
			int t = y0;

			y0 = y1;
			y1 = t;
		}
		for (int y = y0; y <= y1 && y < OLED_HEIGHT; y++) {
			set_pixel_row(y, x, x + 1);
		}
	}

	return push_frame();
}

int oled_draw_bars(const uint8_t *heights, const uint8_t *peaks, int n_bars, uint8_t max_height)
{
	if (!disp_dev) {
		return -ENODEV;
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
			set_pixel_row(y, x0, x_end);
		}

		/* Peak dot: a one-pixel line at the peak height - only visible
		 * once it's above the bar (at the bar's own height it's just
		 * the bar's top row).
		 */
		if (peaks && peaks[b] > 0) {
			uint8_t p = peaks[b];

			if (p > max_height) {
				p = max_height;
			}
			if (p > OLED_HEIGHT) {
				p = OLED_HEIGHT;
			}
			set_pixel_row(OLED_HEIGHT - p, x0, x_end);
		}
	}

	return push_frame();
}
