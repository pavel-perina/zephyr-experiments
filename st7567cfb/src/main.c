#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>

int main() {
    printk("** Starting ST7567 CFB test **\n");

    const struct device *display_dev;
    display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    if (!device_is_ready(display_dev)) {
        printk("Display device not ready!\n");
        return -1;
    }

    // Initialize CFB
    if (display_set_pixel_format(display_dev, PIXEL_FORMAT_MONO10) != 0) {
        printk("Failed to set pixel format\n");
    }

    if (display_set_contrast(display_dev, 0)) {
        printk("Failed to set contrast\n");
    }

    if (cfb_framebuffer_init(display_dev)) {
        printk("CFB init failed!\n");
        return -1;
    }

    cfb_framebuffer_clear(display_dev, true);
    display_blanking_off(display_dev);

    // Print available fonts
    uint8_t font_count = cfb_get_numof_fonts(display_dev);
    printk("Available fonts: %d\n", font_count);
    for (uint8_t i = 0; i < font_count; ++i) {
        uint8_t w, h;
        cfb_get_font_size(display_dev, i, &w, &h);
        printk("Font: %d %02dx%02d\n", i, w, h);
    }

    // Set font (try font 0)
    cfb_framebuffer_set_font(display_dev, 0);

    // Print hello world
    cfb_print(display_dev, "Hello World!", 0, 0);
    cfb_framebuffer_finalize(display_dev);

    printk("Done.\n");
    return 0;
}
