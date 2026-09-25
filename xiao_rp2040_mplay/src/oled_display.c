#include <zephyr/device.h>
#include <zephyr/drivers/display.h>

#include "oled_display.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT 64

/* SSD1306 pages data 8 vertical pixels per byte, so a full-screen buffer is
 * width * height / 8 bytes - 1024 for 128x64. All-zero = all-black in this
 * driver's default pixel format (PIXEL_FORMAT_MONO01, since the shield
 * overlay doesn't set color-inversion).
 */
static uint8_t blank_buf[OLED_WIDTH * OLED_HEIGHT / 8];

int oled_display_init(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(dev)) {
		return -1;
	}

	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(blank_buf),
		.width = OLED_WIDTH,
		.height = OLED_HEIGHT,
		.pitch = OLED_WIDTH,
	};

	return display_write(dev, 0, 0, &desc, blank_buf);
}
