#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

#define TILE_WIDTH  32
#define TILE_HEIGHT 32

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static void test_display(const struct device *display) {
    struct display_buffer_descriptor desc = {
        .width  = TILE_WIDTH,
        .height = TILE_HEIGHT,
        .pitch  = TILE_WIDTH,
    };

    // colors are 0xRRRR RGGG GGGB BBBB = 0x07e0 but there is endianity
    // so green is not 0x07e0, but 0xe007
    static uint16_t buf[TILE_WIDTH * TILE_HEIGHT];
    uint16_t colors[] = {
        0x00f8, // red
        0xe007, // green
        0x1f00, // blue
        0xffff, // white
        0x0000, // black
    };

    for (int i = 0; i < sizeof(colors)/sizeof(uint16_t); ++i) {
        // Fill buffer with color
        for (int j = 0; j < TILE_WIDTH * TILE_HEIGHT; ++j) {
            buf[j] = colors[i];
        }

        display_write(display, i * TILE_WIDTH, 0, &desc, buf);
    }
}

int main(void) {
    LOG_INF("Starting ILI9341 framebuffer test.");

    /* Get display device */
    const struct device *display_dev;
    display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display_dev)) {
        LOG_ERR("Display device not ready");
        return -1;
    }

    test_display(display_dev);

    display_blanking_off(display_dev);

    LOG_INF("Done.");
    return 0;
}
