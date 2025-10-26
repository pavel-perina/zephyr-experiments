#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>

int main() {
    printk("** Starting ST7567 test **\n");
    const struct device *display_dev;
    display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display_dev)) {
        printk("Display device not ready!\n");
        return -1;
    }
    display_blanking_off(display_dev);

    // Check display capabilities
    struct display_capabilities caps;
    display_get_capabilities(display_dev, &caps);
    printk("Display size: %dx%d\n",      caps.x_resolution, caps.y_resolution);
    printk("Current pixel format: %d\n", caps.current_pixel_format);
    printk("Supported formats: 0x%x\n",  caps.supported_pixel_formats);
    printk("Screen info: 0x%x\n",        caps.screen_info);

    // Write 11x8 pixels space invader
    struct display_buffer_descriptor buf_desc = {
        .buf_size = 11, .width = 11, .height = 8, .pitch = 11
    };
    static uint8_t buf[11] = "\x70\x18\x7d\xb6\xbc\x3c\xbc\xb6\x7d\x18\x70";
    display_write(display_dev, 0, 0, &buf_desc, buf);

    printk("Done.\n");
    return 0;
}
