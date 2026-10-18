/*
 * SHT40 temperature and humidity, read every 2 s and printed to the
 * console. The sensor node (label sht4x) comes from boards/<board>.overlay.
 */
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

static const struct device *const sht4x = DEVICE_DT_GET(DT_NODELABEL(sht4x));

/* sensor_value as hundredths, without float printf support */
static void print_centi(const char *name, const struct sensor_value *v, const char *unit)
{
	int32_t c = (int32_t)(sensor_value_to_milli(v) / 10);

	printk("%s %s%d.%02d %s", name, c < 0 ? "-" : "", abs(c) / 100, abs(c) % 100, unit);
}

int main(void)
{
	if (!device_is_ready(sht4x)) {
		printk("SHT40 not ready - check wiring (D14 SDA, D15 SCL, 3V3, GND)\n");
		return 0;
	}

	while (1) {
		struct sensor_value t, rh;
		int err = sensor_sample_fetch(sht4x);

		if (err == 0) {
			sensor_channel_get(sht4x, SENSOR_CHAN_AMBIENT_TEMP, &t);
			sensor_channel_get(sht4x, SENSOR_CHAN_HUMIDITY, &rh);
			printk("[%u s] ", k_uptime_get_32() / 1000);
			print_centi("T", &t, "C, ");
			print_centi("RH", &rh, "%\n");
		} else {
			printk("[%u s] SHT40 error %d\n", k_uptime_get_32() / 1000, err);
		}
		k_msleep(2000);
	}

	return 0;
}
