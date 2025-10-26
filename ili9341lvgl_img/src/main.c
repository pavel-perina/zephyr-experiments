#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>
#include <zephyr/logging/log.h>
#include "holedna.h"

// https://docs.lvgl.io/master/details/main-modules/image.html

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void) {
    LOG_INF("Starting LVGL example");
    /* Get display device */
    const struct device *display_dev;
    display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display_dev)) {
        LOG_ERR("Display device not ready");
        return -1;
    }
    LOG_INF("Display device ready");

    display_blanking_off(display_dev);

    /* Set background color */
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), LV_PART_MAIN);

    LV_IMAGE_DECLARE(holedna);
    lv_obj_t* wp_img = lv_image_create(lv_screen_active());
    lv_image_set_src(wp_img, &holedna);
    lv_obj_align(wp_img, LV_ALIGN_CENTER, 0, 0);

    LOG_INF("LVGL widgets created");

    /* Main loop */
    while (1) {
        lv_timer_handler();
        k_msleep(10);
    }

    return 0;
}

/* vim: set expandtab shiftwidth=4 softtabstop=4 tabstop=4 : */
