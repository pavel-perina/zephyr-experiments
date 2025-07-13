#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <string.h>

// GPIO definitions
static const struct gpio_dt_spec green_led = GPIO_DT_SPEC_GET(DT_ALIAS(led_green), gpios);
static const struct gpio_dt_spec red_led   = GPIO_DT_SPEC_GET(DT_ALIAS(led_red), gpios);
static const struct gpio_dt_spec blue_led   = GPIO_DT_SPEC_GET(DT_ALIAS(led_blue), gpios);

// Logger flag
static atomic_t logger_enabled = ATOMIC_INIT(1);
#define TIMEOUT 60

// Shell command: control green LED
static int cmd_led(const struct shell *shell, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_print(shell, "Usage: led <on|off>");
        return -EINVAL;
    }

    if (strcmp(argv[1], "on") == 0) {
        gpio_pin_set_dt(&green_led, 1);
        shell_print(shell, "Green LED ON");
    } else if (strcmp(argv[1], "off") == 0) {
        gpio_pin_set_dt(&green_led, 0);
        shell_print(shell, "Green LED OFF");
    } else {
        shell_error(shell, "Invalid argument: %s", argv[1]);
        return -EINVAL;
    }

    return 0;
}

// Shell command: control logger
static int cmd_logger(const struct shell *shell, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_print(shell, "Usage: logger <on|off>");
        return -EINVAL;
    }

    if (strcmp(argv[1], "on") == 0) {
        atomic_set(&logger_enabled, 1);
        shell_print(shell, "Logger ON");
    } else if (strcmp(argv[1], "off") == 0) {
        atomic_set(&logger_enabled, 0);
        shell_print(shell, "Logger OFF");
    } else {
        shell_error(shell, "Invalid argument: %s", argv[1]);
        return -EINVAL;
    }

    return 0;
}


static int cmd_timers(const struct shell *shell, size_t argc, char **argv)
{
    int64_t uptime_ms = k_uptime_get();
    shell_print(shell, "k_uptime_get() returned %lld [ms]", uptime_ms);

    int64_t uptime_ticks = k_uptime_ticks();
    shell_print(shell, "k_uptime_ticks() returned %lld", uptime_ticks);

    uint64_t uptime_us = k_ticks_to_us_floor64(uptime_ticks);
    shell_print(shell, "k_ticks_to_us_floor64() returned %llu [us]", uptime_us);

    return 0;
}


SHELL_CMD_REGISTER(led,    NULL, "Toggle green LED: led <on|off>", cmd_led);
SHELL_CMD_REGISTER(logger, NULL, "Toggle logger: logger <on|off>", cmd_logger);
SHELL_CMD_REGISTER(timers, NULL, "Show timers", cmd_timers);

// Logger thread: blinks red LED every 50 ms when enabled
void logger_thread(void) {
    k_sleep(K_MSEC(1000*TIMEOUT));
    gpio_pin_set_dt(&blue_led,  0);
    while(1) {
        k_sleep(K_MSEC(50));
        if (atomic_get(&logger_enabled)) {
            gpio_pin_toggle_dt(&red_led);
        }
    }
}

K_THREAD_DEFINE(logger_thread_id, 1024, logger_thread, NULL, NULL, NULL, 5, 0, 0);

int main(void)
{
    // Initialize LEDs
    if (!device_is_ready(green_led.port) || !device_is_ready(red_led.port)) {
        printk("LED devices not ready\n");
        return -1;
    }

    gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&red_led,   GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&blue_led,  GPIO_OUTPUT_INACTIVE);
    gpio_pin_set_dt(&blue_led,  1);

    printk("Type logger off in 60 seconds to disable logger\n");

    // NOTE: here we have shell thread and logger thread running, 
    //       program won't exit
    return 0;
}