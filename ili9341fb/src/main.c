#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static void test_display(const struct device *display) {
    struct display_buffer_descriptor desc = {
        .width  = 10,
        .height = 10,
        .pitch  = 10,
    };

    // colors are 0xRRRR RGGG GGGB BBBB = 0x07e0 but there is endianity
    // so green is not 0x07e0, but 0xe007
    uint16_t green = 0xe007;
    uint16_t red   = 0x00f8;
    uint16_t blue  = 0x1f00;
    static uint16_t buf[100]; // 10x10 = 100 pixels

    for (int i = 0; i < 100; i++)
        buf[i] = red;
    display_write(display, 0, 0, &desc, buf);

    for (int i = 0; i < 100; i++)
        buf[i] = green;
    display_write(display, 10, 0, &desc, buf);

    for (int i = 0; i < 100; i++)
        buf[i] = blue;
    display_write(display, 20, 0, &desc, buf);

    for (int i = 0; i < 100; i++)
        buf[i] = 0xffff;
    display_write(display, 30, 0, &desc, buf);
}

int main(void) {
    LOG_INF("Starting LVGL Hello World example");

    /* Get display device */
    const struct device *display_dev;
    display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display_dev)) {
        LOG_ERR("Display device not ready");
        return -1;
    }

    test_display(display_dev);

    LOG_INF("Display device ready");

    /* Display blanking off */
    display_blanking_off(display_dev);

    LOG_INF("Done.");
    return 0;
}

