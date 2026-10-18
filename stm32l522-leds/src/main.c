#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* LED node labels come from the board DTS (nucleo_l552ze_q-common.dtsi) */
static const struct {
    struct gpio_dt_spec gpio;
    const char *name;
} leds[] = {
    { GPIO_DT_SPEC_GET(DT_NODELABEL(red_led_1), gpios), "red" },
    { GPIO_DT_SPEC_GET(DT_NODELABEL(green_led_1), gpios), "green" },
    { GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led_1), gpios), "blue" },
};

int main(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
        if (!gpio_is_ready_dt(&leds[i].gpio) ||
            gpio_pin_configure_dt(&leds[i].gpio, GPIO_OUTPUT_INACTIVE) < 0) {
            printk("LED %s: setup failed\n", leds[i].name);
            return 0;
        }
    }

    for (size_t i = 0;; i = (i + 1) % ARRAY_SIZE(leds)) {
        printk("%s\n", leds[i].name);
        gpio_pin_set_dt(&leds[i].gpio, 1);
        k_msleep(500);
        gpio_pin_set_dt(&leds[i].gpio, 0);
    }

    return 0;
}
