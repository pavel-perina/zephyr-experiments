/*
 * SCD41 (CO2, single-shot) + SHT40 (T/RH) on a XIAO BLE, measured every
 * `interval` seconds (default 30) by a logger thread, printed to the USB
 * console. Shell commands:
 *   logger now             - measure immediately (extra, schedule unchanged)
 *   logger interval <s>    - set the interval (6..3600 s)
 *   logger toffset [C]     - show / set the temperature offset the app applies
 *                            to the SCD41 (compensated in software)
 * plus Zephyr's own `sensor get <device>` (SENSOR_SHELL).
 */
#include <math.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/scd4x.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>

/* SCD41 temperature offset - compensated in software.
 *
 * The sensor subtracts a stored offset (factory default 4 C) from its
 * temperature to cancel self-heating in *periodic* mode inside a typical
 * enclosure. In single-shot mode it barely heats, so 4 C over-corrects: the
 * first run read ~4.3 C below the SHT40, and a much higher %RH (computed
 * from that too-low temperature).
 *
 * Changing the offset in the sensor doesn't work with Zephyr's driver in
 * single-shot mode: sensor_attr_set() writes it to the sensor's RAM and
 * then sends power_down, and the next wake_up evidently reloads the stored
 * 4 C from EEPROM (persist_settings() starts with a wake_up too, so it
 * would save the 4 C again). So the sensor keeps its stored offset; the app
 * reads it once at boot and corrects every reading:
 *   T_true  = T_reported + sensor_offset - app_offset
 *   RH_true = RH_reported * es(T_reported) / es(T_true)
 * es() = Magnus saturation vapour pressure: the sensor computed RH against
 * the wrong temperature; this redoes it for the right one.
 */
#define APP_TOFFSET_MILLI_C_DEFAULT 0

static float sensor_toffset_c = 4.0f;   /* read from the sensor at boot */
static atomic_t app_toffset_mc = ATOMIC_INIT(APP_TOFFSET_MILLI_C_DEFAULT);

/* Saturation vapour pressure over water, hPa (Magnus, Sonntag 1990 coeffs). */
static float es_hpa(float t_c)
{
	return 6.112f * expf(17.62f * t_c / (243.12f + t_c));
}

static const struct device *const scd41 = DEVICE_DT_GET(DT_NODELABEL(scd41));
static const struct device *const sht4x = DEVICE_DT_GET(DT_NODELABEL(sht4x));

static atomic_t interval_s = ATOMIC_INIT(30);
K_SEM_DEFINE(measure_now, 0, 1);

/* sensor_value as "-12.34" without float printf support (32-bit maths:
 * fine for CO2 ppm, degrees C and %RH).
 */
static void fmt(char *buf, size_t len, const struct sensor_value *v, int decimals)
{
	int32_t div = 1;

	for (int i = 0; i < decimals; i++) {
		div *= 10;
	}
	/* value * 10^decimals, truncated: val1 * div + val2 / (10^6 / div) */
	int32_t scaled = v->val1 * div + v->val2 / (1000000 / div);
	bool neg = scaled < 0;
	int32_t a = neg ? -scaled : scaled;

	if (decimals == 0) {
		snprintk(buf, len, "%s%d", neg ? "-" : "", (int)a);
	} else {
		snprintk(buf, len, "%s%d.%0*d", neg ? "-" : "", (int)(a / div), decimals,
			 (int)(a % div));
	}
}

/* Print one line through the shell (not printk), so it doesn't get mixed
 * into the prompt or a command being typed.
 */
#define out(...) shell_print(shell_backend_uart_get_ptr(), __VA_ARGS__)

static void measure(void)
{
	struct sensor_value co2, t1, rh1, t1raw, rh1raw, t2, rh2;
	char a[24], b[24], c[24], d[24], e[24], f[24], g[24];
	uint32_t start = k_uptime_get_32();

	/* SCD41 single-shot: blocks ~5 s while the sensor measures. */
	int err1 = sensor_sample_fetch(scd41);

	if (err1 == 0) {
		sensor_channel_get(scd41, SENSOR_CHAN_CO2, &co2);
		sensor_channel_get(scd41, SENSOR_CHAN_AMBIENT_TEMP, &t1raw);
		sensor_channel_get(scd41, SENSOR_CHAN_HUMIDITY, &rh1raw);

		/* software offset compensation - see the comment at the top */
		float tr = sensor_value_to_float(&t1raw);
		float rhr = sensor_value_to_float(&rh1raw);
		float tt = tr + sensor_toffset_c - atomic_get(&app_toffset_mc) / 1000.0f;
		float rht = rhr * es_hpa(tr) / es_hpa(tt);

		sensor_value_from_float(&t1, tt);
		sensor_value_from_float(&rh1, rht > 100.0f ? 100.0f : rht);
	}

	int err2 = sensor_sample_fetch(sht4x);

	if (err2 == 0) {
		sensor_channel_get(sht4x, SENSOR_CHAN_AMBIENT_TEMP, &t2);
		sensor_channel_get(sht4x, SENSOR_CHAN_HUMIDITY, &rh2);
	}

	char line[192];
	int n = snprintk(line, sizeof(line), "[%u s] ", k_uptime_get_32() / 1000);

	if (err1 == 0) {
		fmt(a, sizeof(a), &co2, 0);
		fmt(b, sizeof(b), &t1, 2);
		fmt(c, sizeof(c), &rh1, 1);
		fmt(f, sizeof(f), &t1raw, 2);
		fmt(g, sizeof(g), &rh1raw, 1);
		n += snprintk(line + n, sizeof(line) - n,
			      "SCD41 CO2 %s ppm, %s C, %s %%RH [raw %s C %s %%] | ", a, b, c, f, g);
	} else {
		n += snprintk(line + n, sizeof(line) - n, "SCD41 error %d | ", err1);
	}
	if (err2 == 0) {
		fmt(d, sizeof(d), &t2, 2);
		fmt(e, sizeof(e), &rh2, 1);
		n += snprintk(line + n, sizeof(line) - n, "SHT40 %s C, %s %%RH", d, e);
	} else {
		n += snprintk(line + n, sizeof(line) - n, "SHT40 error %d", err2);
	}
	snprintk(line + n, sizeof(line) - n, "  (%u ms)", k_uptime_get_32() - start);
	out("%s", line);
}

/* Read the offset stored in the sensor (wakes it briefly; the driver puts
 * it back to sleep) - the base for the software compensation.
 */
static void scd41_read_sensor_toffset(void)
{
	struct sensor_value v;
	char buf[24];

	if (sensor_attr_get(scd41, SENSOR_CHAN_ALL,
			    (enum sensor_attribute)SENSOR_ATTR_SCD4X_TEMPERATURE_OFFSET, &v) == 0) {
		sensor_toffset_c = sensor_value_to_float(&v);
		fmt(buf, sizeof(buf), &v, 2);
		out("SCD41 stored temperature offset: %s C (compensated in software)", buf);
	} else {
		out("SCD41 offset read failed - assuming the factory 4 C");
	}
}

static void logger_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!device_is_ready(scd41)) {
		out("SCD41 not ready");
	} else {
		scd41_read_sensor_toffset();
	}
	if (!device_is_ready(sht4x)) {
		out("SHT40 not ready");
	}

	/* Fixed schedule: measurement n starts at n * interval, however long the
	 * (~5 s) measurement takes - not interval + 5 s apart. `logger now`
	 * takes an extra measurement without shifting the schedule.
	 */
	int64_t next = k_uptime_get() + (int64_t)atomic_get(&interval_s) * 1000;

	while (1) {
		int64_t wait = next - k_uptime_get();

		if (k_sem_take(&measure_now, K_MSEC(wait > 0 ? wait : 0)) == 0) {
			measure();   /* `logger now` */
			continue;
		}
		measure();
		next += (int64_t)atomic_get(&interval_s) * 1000;
		if (next < k_uptime_get()) {
			next = k_uptime_get();   /* fell behind (e.g. interval shortened) */
		}
	}
}
K_THREAD_DEFINE(logger_tid, 2048, logger_thread, NULL, NULL, NULL, 7, 0, 1000);

static int cmd_now(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	k_sem_give(&measure_now);
	shell_print(sh, "measuring (SCD41 single shot takes ~5 s)");
	return 0;
}

static int cmd_interval(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "interval %ld s", (long)atomic_get(&interval_s));
		return 0;
	}
	long s = strtol(argv[1], NULL, 10);

	if (s < 6 || s > 3600) {
		shell_error(sh, "interval must be 6..3600 s (a single shot takes ~5 s)");
		return -EINVAL;
	}
	atomic_set(&interval_s, s);
	shell_print(sh, "interval %ld s (from the next scheduled measurement)", s);
	return 0;
}

static int cmd_toffset(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2) {
		/* degrees C, e.g. 0, 1.5 - what the app subtracts instead of the
		 * sensor's stored offset
		 */
		double c = strtod(argv[1], NULL);

		if (c < -10 || c > 20) {
			shell_error(sh, "offset must be -10..20 C");
			return -EINVAL;
		}
		atomic_set(&app_toffset_mc, (atomic_val_t)lround(c * 1000));
	}
	shell_print(sh, "app temperature offset %d.%03d C (sensor stores %d.%02d C)",
		    (int)(atomic_get(&app_toffset_mc) / 1000),
		    (int)labs(atomic_get(&app_toffset_mc) % 1000),
		    (int)sensor_toffset_c, (int)(sensor_toffset_c * 100) % 100);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_logger,
	SHELL_CMD(now, NULL, "Measure immediately", cmd_now),
	SHELL_CMD_ARG(interval, NULL, "Show or set interval: interval [6..3600]", cmd_interval, 1, 1),
	SHELL_CMD_ARG(toffset, NULL, "Show or set the app's SCD41 temperature offset: toffset [C]",
		      cmd_toffset, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(logger, &sub_logger, "SCD41/SHT40 logger", NULL);

int main(void)
{
	return 0;
}
