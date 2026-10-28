/*
 * VEML7700 ambient light, read once per second and printed to the console.
 * The sensor node (label veml7700) comes from boards/<board>.overlay.
 *
 * Auto-ranging and the non-linearity correction follow Vishay's application
 * note "Designing the VEML7700 Into an Application" (doc. 84323): start at
 * the lowest gain, raise gain before integration time, use only gain 1/8
 * and 1/4 for bright light and correct those readings with the note's
 * polynomial. Lux is computed here from the raw counts - the driver's own
 * lux value is whole lux only.
 *
 * The driver measures single-shot and waits twice the integration time, so
 * 400 ms is the longest integration time that fits in a 1 s period.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/veml7700.h>

static const struct device *const veml7700 = DEVICE_DT_GET(DT_NODELABEL(veml7700));

/* Range steps from least to most sensitive, each about 2x (one 4x) the previous. */
static const struct range {
	enum veml7700_als_gain gain;
	enum veml7700_als_it it;
	float res;		/* lx per count, from the datasheet's resolution table */
	const char *name;
} ranges[] = {
	{VEML7700_ALS_GAIN_1_8, VEML7700_ALS_IT_25, 1.8432f, "1/8 25 ms"},
	{VEML7700_ALS_GAIN_1_8, VEML7700_ALS_IT_50, 0.9216f, "1/8 50 ms"},
	{VEML7700_ALS_GAIN_1_8, VEML7700_ALS_IT_100, 0.4608f, "1/8 100 ms"},
	{VEML7700_ALS_GAIN_1_4, VEML7700_ALS_IT_100, 0.2304f, "1/4 100 ms"},
	{VEML7700_ALS_GAIN_1, VEML7700_ALS_IT_100, 0.0576f, "1 100 ms"},
	{VEML7700_ALS_GAIN_2, VEML7700_ALS_IT_100, 0.0288f, "2 100 ms"},
	{VEML7700_ALS_GAIN_2, VEML7700_ALS_IT_200, 0.0144f, "2 200 ms"},
	{VEML7700_ALS_GAIN_2, VEML7700_ALS_IT_400, 0.0072f, "2 400 ms"},
};

#define RANGE_START   2		/* gain 1/8, 100 ms: the note's starting point */
#define RANGE_LOW_GAIN 3	/* steps 0..3 use gain 1/8 or 1/4 */

/*
 * Keep the ALS counts in [COUNTS_LOW, COUNTS_HIGH]. 10000 is the note's
 * threshold for shortening the integration time; it also keeps gain 1 and 2
 * below ~580 lx, inside their linear range. The band is wider than the
 * largest step (4x), so a range change never lands outside it.
 */
#define COUNTS_LOW  1000
#define COUNTS_HIGH 10000
#define COUNTS_SAT  65535

static int set_range(int r)
{
	struct sensor_value v = {.val1 = ranges[r].gain};
	int err = sensor_attr_set(veml7700, SENSOR_CHAN_LIGHT,
				  (enum sensor_attribute)SENSOR_ATTR_VEML7700_GAIN, &v);

	if (err == 0) {
		v.val1 = ranges[r].it;
		err = sensor_attr_set(veml7700, SENSOR_CHAN_LIGHT,
				      (enum sensor_attribute)SENSOR_ATTR_VEML7700_ITIME, &v);
	}
	return err;
}

/* The most sensitive range in which the same light gives at most COUNTS_HIGH counts. */
static int pick_range(int r, uint32_t counts)
{
	float lux = counts * ranges[r].res;
	int best = 0;

	for (int i = 0; i < ARRAY_SIZE(ranges); i++) {
		if (lux / ranges[i].res <= COUNTS_HIGH) {
			best = i;
		}
	}
	return best;
}

/* Application note, fig. 10 and 24: x is the uncorrected lux, gain 1/8 and 1/4 only. */
static float correct_lux(float x)
{
	return (((6.0135e-13f * x - 9.3924e-9f) * x + 8.1488e-5f) * x + 1.0023f) * x;
}

static void print_milli(float lux)
{
	uint32_t m = (uint32_t)(lux * 1000.0f + 0.5f);

	printk("%u.%03u", m / 1000, m % 1000);
}

int main(void)
{
	if (!device_is_ready(veml7700)) {
		printk("VEML7700 not ready - check wiring (D4 SDA, D5 SCL, 3V3, GND)\n");
		return 0;
	}

	int r = RANGE_START;
	int64_t next = k_uptime_get();

	set_range(r);

	while (1) {
		struct sensor_value als, white;
		int err = sensor_sample_fetch(veml7700);

		if (err != 0) {
			printk("[%u s] VEML7700 error %d\n", k_uptime_get_32() / 1000, err);
			goto wait;
		}

		sensor_channel_get(veml7700, (enum sensor_channel)SENSOR_CHAN_VEML7700_RAW_COUNTS,
				   &als);
		sensor_channel_get(veml7700,
				   (enum sensor_channel)SENSOR_CHAN_VEML7700_WHITE_RAW_COUNTS, &white);

		if (als.val1 >= COUNTS_SAT && r > 0) {
			/* Saturated: no idea how bright, go to the least sensitive
			 * range and measure again right away (25 ms takes ~55 ms).
			 */
			r = 0;
			set_range(r);
			continue;
		}

		float lux = als.val1 * ranges[r].res;

		printk("[%u s] ", k_uptime_get_32() / 1000);
		if (r <= RANGE_LOW_GAIN) {
			print_milli(correct_lux(lux));
			printk(" lx (uncorrected ");
			print_milli(lux);
			printk(")");
		} else {
			print_milli(lux);
			printk(" lx");
		}
		printk(", ALS %d, white %d, range %s%s\n", als.val1, white.val1, ranges[r].name,
		       als.val1 >= COUNTS_SAT ? ", SATURATED" : "");

		if (als.val1 < COUNTS_LOW || als.val1 > COUNTS_HIGH) {
			int nr = pick_range(r, als.val1);

			if (nr != r) {
				r = nr;
				set_range(r);
			}
		}

wait:
		next += 1000;
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}

	return 0;
}
