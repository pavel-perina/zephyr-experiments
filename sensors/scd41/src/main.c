#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

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

int main(void) {
    printk("Starting\n");
    if (!device_is_ready(sensor)) {
        printk("Sensor not ready!\n");
        return -1;
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
