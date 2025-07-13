/* Demonstration code using XIAO expansion board with
 * PCF8563 RTC and SSD1306 display without LVGL
 * (Light and Versatile Graphics Library)
 * and universal RTC driver which does not work with wrong RTC time.
 * 
 * This simplifies the code, but somewhat violates best practices
 * and portability, because these things are defined in the code
 * rather than the device tree.
 *
 * Pavel Perina, 2025-07-13
 */

#include "font.h"

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/display.h>

#define PCF8563_I2C_ADDR    0x51
#define PCF8563_REG_SECONDS 0x02

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define DISPLAY_FB_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)

const struct device *display = DEVICE_DT_GET_ONE(solomon_ssd1306fb);
const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(xiao_i2c));

struct display_buffer_descriptor display_descriptor = {
    .width = DISPLAY_WIDTH,
    .height = DISPLAY_HEIGHT,
    .pitch = DISPLAY_WIDTH,
    .buf_size = DISPLAY_FB_SIZE
};

uint8_t framebuffer[DISPLAY_FB_SIZE] = { 0 };

void fb_clear(void) 
{
    memset(framebuffer, 0, DISPLAY_FB_SIZE);
}

void fb_write(int row, int x, const char *text) {
    int dst_index = row * DISPLAY_WIDTH + x;
    while (*text) {
        uint16_t ch = (uint8_t)(*text);
        int src_index = ch * 6;
        for (int i = 0; i < 6; i++) {
            if (dst_index >= DISPLAY_FB_SIZE)
                return;
            framebuffer[dst_index++] = font_data[src_index + i];
        }
        text++;
    }
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}


int main(void)
{
    if (!device_is_ready(i2c_dev)) {
        printk("I2C device not ready\n");
        return -1;
    }
    if (!device_is_ready(display)) {
        printk("Display device not ready\n");
        return -1;
    }

    char buf[32];
    uint8_t time_data[7];

    while (1) {
        // Read 6 bytes: seconds, minutes, hours, day, month, year
        int ret = i2c_burst_read(i2c_dev, PCF8563_I2C_ADDR, PCF8563_REG_SECONDS, time_data, 7);
        if (ret < 0) {
            fb_write(0,0,"I2C read failed");
            snprintf(buf, sizeof(buf), "Error: %d", ret);
            fb_write(1,0, buf);
        } else {
            fb_write(0, 0, "Hello Zephyr!");

            snprintf(buf, sizeof(buf), "Date: 20%02d-%02d-%02d", 
			    bcd_to_bin(time_data[6]), 
			    bcd_to_bin(time_data[5] & 0x0f),
			    bcd_to_bin(time_data[3] & 0x3f));
            fb_write(1, 0, buf);

            snprintf(buf, sizeof(buf), "Time:   %02d:%02d:%02d",
			    bcd_to_bin(time_data[2] & 0x3f), 
			    bcd_to_bin(time_data[1] & 0x7f), 
			    bcd_to_bin(time_data[0] & 0x7f));
            fb_write(2, 0, buf);

	    if ((time_data[0] & 0x80) != 0) {
		    fb_write(3, 0, "Bad integrity");
	    }
        }
	display_write(display, 0, 0, &display_descriptor, framebuffer);
	display_blanking_off(display);

        k_sleep(K_SECONDS(1));
    }
    return 0;
}

