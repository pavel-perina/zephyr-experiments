#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/uart.h>

const struct device *const sensor = DEVICE_DT_GET(DT_ALIAS(env_sensor));

int sensor_print_helper(const struct device *const sensor, int channel, const char *name) {
    struct sensor_value value;
    int ret = sensor_channel_get(sensor, channel, &value);
    if (ret == 0) {
        printk("%12s : %d.%06d\n", name, value.val1, value.val2);
    } else {
        printk("%12s : N/A\n", name);
    }
    return ret;
}

/* With a USB CDC console (RP2040/Pico), output printed before a terminal
 * opens the port is lost - wait until the host raises DTR. Consoles without
 * line control (a real UART, e.g. ESP32 via its USB-serial bridge) skip it.
 */
static void wait_for_terminal(void) {
#if defined(CONFIG_UART_LINE_CTRL)
    const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint32_t dtr = 0;

    while (uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr) == 0 && !dtr) {
        k_msleep(100);
    }
#endif
}

int main(void) {
    wait_for_terminal();
    printk("Starting\n");

    /* Keep saying so instead of returning, so the console always shows
     * what's wrong (e.g. sensor not wired to these pins).
     */
    while (!device_is_ready(sensor)) {
        printk("Sensor not ready!\n");
        k_sleep(K_SECONDS(5));
    }

    while (1) {
        printk("Loop\n");
        int ret= sensor_sample_fetch(sensor);
        if (ret != 0) {
            printk("sensor_sample_fetch returned %d\n", ret);
            continue;
        }

        printk("---\n");
        sensor_print_helper(sensor, SENSOR_CHAN_AMBIENT_TEMP, "Temperature");
        sensor_print_helper(sensor, SENSOR_CHAN_PRESS,        "Pressure");
        sensor_print_helper(sensor, SENSOR_CHAN_HUMIDITY,     "Humidity");
        sensor_print_helper(sensor, SENSOR_CHAN_CO2, "CO2");

        k_sleep(K_SECONDS(2));
    }
    return 0;
}
