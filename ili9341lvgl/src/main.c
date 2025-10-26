#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>
#include <zephyr/logging/log.h>

// https://docs.lvgl.io/9.4/examples.html

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static int display_caps(const struct device *dev) {
#if 0
    struct display_capabilities caps;

    if (display_get_capabilities(dev, &caps) != 0) {
        LOG_ERR("Can't get display capabilities");
        return -1;
    }

    LOG_INF("Display: %dx%d %dbpp", caps.x_resolution, caps.y_resolution, caps.current_pixel_format);
#endif
    return 0;
}

int main(void) {
    LOG_INF("Starting LVGL Hello World example");
    
    /* Get display device */
    const struct device* display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display_dev)) {
        LOG_ERR("Display device not ready");
        return -1;
    }
    LOG_INF("Display device ready");

    display_caps(display_dev);
    display_blanking_off(display_dev);

    /* Create the main screen */
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_scr_load(screen);

    /* Set background color */
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);

    /* Create a container box with border (frame around display) */
    lv_obj_t *border_box = lv_obj_create(screen);
    lv_obj_set_size(border_box, LV_HOR_RES - 10, LV_VER_RES - 10);
    lv_obj_center(border_box);
    
    /* Style the border */
    lv_obj_set_style_border_color(border_box, lv_color_hex(0xaab0ad), LV_PART_MAIN);
    lv_obj_set_style_border_width(border_box, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(border_box, lv_color_hex(0xf6f2ee), LV_PART_MAIN);
    lv_obj_set_style_radius(border_box, 15, LV_PART_MAIN);

    /* Create label for "Hello World" */
    lv_obj_t* label = lv_label_create(border_box);
    lv_label_set_text(label, "Hello World!");

    /* Style the label */
    lv_obj_set_style_text_color(label, lv_color_hex(0x4863b6), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, LV_PART_MAIN);

    /* Center the label in the border box */
    lv_obj_center(label);

    LOG_INF("LVGL widgets created");

    /* Main loop */
    while (1) {
        lv_timer_handler();
        
        k_msleep(10);
    }

    return 0;
}

/* vim: set expandtab shiftwidth=4 softtabstop=4 tabstop=4 : */
