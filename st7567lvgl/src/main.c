#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>

int main(void)
{
    printk("Starting\n");
    k_msleep(100);
    const struct device *display_dev;
    display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    
    if (!device_is_ready(display_dev)) {
        printk("Display not ready!\n");
        return -1;
    }

    // Create UI
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0); // Background: 0 (backlight)
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0); // Text: 1 (pixels on)

    display_blanking_off(display_dev);
    printk("Entering loop\n");
    while (1) {
        if (lv_timer_handler() != LV_RES_OK) {
            printk("Timer handler failed.\n");
        }
        k_msleep(50);
    }
    
    return 0;
}

/* vim: set expandtab shiftwidth=4 softtabstop=4 tabstop=4 : */

