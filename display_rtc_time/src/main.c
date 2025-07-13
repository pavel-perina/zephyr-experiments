#include "font.h"

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/display.h>

/* Defined in 
 * 1. ~/zephyrproject/zephyr/boards/shields/seeed_xiao_expansion_board/seeed_xiao_expansion_board.overlay
 * 2. rpi_pico.overlay
 */
const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc));
const struct device *display = DEVICE_DT_GET_ONE(solomon_ssd1306fb);

static int set_date_time(const struct device *rtc)
{
	int ret = 0;
	struct rtc_time tm = {
		.tm_year = 25,
		.tm_mon = 07,
		.tm_mday = 02,
		.tm_hour = 23,
		.tm_min = 30,
		.tm_sec = 00,
	};

	ret = rtc_set_time(rtc, &tm);
	if (ret < 0) {
		printk("Cannot write date time: %d\n", ret);
		return ret;
	}
	return ret;
}

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define DISPLAY_FB_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)

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



int main(void)
{
    /* Get the RTC device */
    if (!device_is_ready(rtc)) {
        printk("RTC device not ready\n");
        return -1;
    }
    if (!device_is_ready(display)) {
        printk("Display device not ready\n");
        return -1;
    }

    printk("RTC initialized\n");

    /* rtc_get_time returns -61 when time is invalid */
    set_date_time(rtc); 

    char buf[32];
    while (1) {
        struct rtc_time current_time;    
        /* Read current time from RTC */
        const int ret = rtc_get_time(rtc, &current_time);
        if (ret < 0) {
            fb_write(0,0,"rtc_get_time failed");
            snprintf(buf, sizeof(buf), "code: %d", ret);
            fb_write(1,0, buf);
            display_write(display, 0, 0, &display_descriptor, framebuffer);
            display_blanking_off(display);
        } else {
            fb_write(0, 0, "Hello Zephyr!");

            snprintf(buf, sizeof(buf),
                    "Date: %04d-%02d-%02d",
                    current_time.tm_year + 2000,
                    current_time.tm_mon,
                    current_time.tm_mday);
            fb_write(1, 0, buf);

            snprintf(buf, sizeof(buf),
                    "Time:   %02d:%02d:%02d",
                    current_time.tm_hour,
                    current_time.tm_min,
                    current_time.tm_sec);
            fb_write(2, 0, buf);

            display_write(display, 0, 0, &display_descriptor, framebuffer);
            display_blanking_off(display);
        }

        k_sleep(K_SECONDS(1));
    }
    return 0;
}
