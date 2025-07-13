#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>

/* Defined in 
 * 1. ~/zephyrproject/zephyr/boards/shields/seeed_xiao_expansion_board/seeed_xiao_expansion_board.overlay
 * 2. rpi_pico.overlay
 */
const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc));


static int set_date_time(const struct device *rtc)
{
	int ret = 0;
	struct rtc_time tm = {
		.tm_year = 25,
		.tm_mon = 06,
		.tm_mday = 13,
		.tm_hour = 1,
		.tm_min = 23,
		.tm_sec = 45,
	};

	ret = rtc_set_time(rtc, &tm);
	if (ret < 0) {
		printk("Cannot write date time: %d\n", ret);
		return ret;
	}
	return ret;
}

int main(void)
{
    /* Get the RTC device */
    if (!device_is_ready(rtc)) {
        printk("RTC device not ready\n");
        return -1;
    }

    printk("RTC initialized\n");

    /* rtc_get_time returns -61 when time is invalid */
    /* set_date_time(rtc); */

    while (1) {
        struct rtc_time current_time;    
        /* Read current time from RTC */
        const int ret = rtc_get_time(rtc, &current_time);
        if (ret < 0) {
            printk("Failed to read RTC time, code=%d\n", ret);
        } else {
            /* Display the time */
            printk("Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                    current_time.tm_year + 2000,
                    current_time.tm_mon,
                    current_time.tm_mday,
                    current_time.tm_hour,
                    current_time.tm_min,
                    current_time.tm_sec);
        }

        k_sleep(K_SECONDS(2));
    }
    return 0;
}
