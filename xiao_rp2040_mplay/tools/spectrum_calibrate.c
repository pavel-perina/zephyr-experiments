/*
 * Host-side calibration harness for spectrum.c's scaling constants
 * (ENVELOPE_MAX for fine mode, LOG_TOP_Q4/LOG_RANGE_Q4 for wide mode).
 *
 * Compiles the real src/mod_player.c and src/spectrum.c for the host,
 * plays each MOD given on the command line through them at the device's
 * cadence - 256-sample buffers into spectrum_accumulate(), one
 * spectrum_process() per 40ms (7.5 buffers) like spectrum_thread() - and
 * prints percentiles of the smoothed envelopes, per mode, over every band
 * and frame.
 *
 * Build and run (from this directory):
 *   cc -O2 -I../src -o spectrum_calibrate spectrum_calibrate.c -lm
 *   ./spectrum_calibrate ../mods/AXEL_F.MOD ../mods/ELYSIUM.MOD ...
 * (normally every file in ../mods, via a shell glob)
 */
#include <stdio.h>
#include <stdlib.h>

#include "../src/mod_player.c"
#include "../src/spectrum.c"

#define BUF_LEN       256
#define SECONDS_MAX   600   /* safety cap per song */
#define SAMPLE_RATE   48000

static int cmp_int32(const void *a, const void *b)
{
	int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;

	return (x > y) - (x < y);
}

/* Per-band samples for the wide layout, for the per-band summary. */
static int32_t *band_vals[SPECTRUM_MAX_BANDS];
static size_t band_n[SPECTRUM_MAX_BANDS], band_cap[SPECTRUM_MAX_BANDS];

static void push_band(int band, int32_t v)
{
	if (band_n[band] == band_cap[band]) {
		band_cap[band] = band_cap[band] ? band_cap[band] * 2 : 1 << 12;
		band_vals[band] = realloc(band_vals[band], band_cap[band] * sizeof(int32_t));
	}
	band_vals[band][band_n[band]++] = v;
}

static int32_t *vals;
static size_t n_vals, cap_vals;

static void push(int32_t v)
{
	if (n_vals == cap_vals) {
		cap_vals = cap_vals ? cap_vals * 2 : 1 << 16;
		vals = realloc(vals, cap_vals * sizeof(*vals));
	}
	vals[n_vals++] = v;
}

/* fine: raw envelope values (compare with ENVELOPE_MAX); wide: tilted
 * log2_q4 values (compare with LOG_TOP_Q4 / LOG_TOP_Q4 - LOG_RANGE_Q4).
 */
static void report(const char *label)
{
	static const double pct[] = { 10, 50, 90, 99, 99.9, 100 };

	qsort(vals, n_vals, sizeof(*vals), cmp_int32);
	printf("%-5s %zu samples:", label, n_vals);
	for (size_t i = 0; i < sizeof(pct) / sizeof(pct[0]); i++) {
		size_t idx = (size_t)(pct[i] / 100.0 * (n_vals - 1));
		int32_t v = vals[idx];

		printf("  p%g=%d", pct[i], v);
	}
	printf("\n");
	n_vals = 0;
}

static void run(const char *path, enum spectrum_mode mode)
{
	FILE *f = fopen(path, "rb");

	if (!f) {
		perror(path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);

	fseek(f, 0, SEEK_SET);
	uint8_t *data = malloc(len);

	if (fread(data, 1, len, f) != (size_t)len) {
		perror(path);
		exit(1);
	}
	fclose(f);

	static struct PlayerState ps;

	memset(&ps, 0, sizeof(ps));
	if (mod_player_init(&ps, data, len) != 0) {
		fprintf(stderr, "%s: mod_player_init failed\n", path);
		exit(1);
	}

	spectrum_set_mode(mode);
	memset(ring, 0, sizeof(ring));

	int16_t buf[BUF_LEN];
	double t_next_frame = 0.04;
	int32_t env[SPECTRUM_MAX_BANDS];
	long buffers = 0;

	while (ps.restarts == 0 && buffers * BUF_LEN < (long)SECONDS_MAX * SAMPLE_RATE) {
		mod_player_produce(&ps, buf, BUF_LEN);
		spectrum_accumulate(buf, BUF_LEN);
		buffers++;
		double t = (double)buffers * BUF_LEN / SAMPLE_RATE;

		if (t >= t_next_frame) {
			t_next_frame += 0.04;
			spectrum_process();
			spectrum_get_raw_envelope(env);
			for (int i = 0; i < spectrum_band_count(); i++) {
				if (mode == SPECTRUM_MODE_WIDE) {
					/* the value get_levels() maps to pixels */
					push(wide_log_q4(i));
					push_band(i, wide_log_q4(i));
				} else {
					push(env[i]);
				}
			}
		}
	}
	free(data);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s file.mod...\n", argv[0]);
		return 1;
	}
	for (int m = 0; m < 2; m++) {
		enum spectrum_mode mode = m ? SPECTRUM_MODE_WIDE : SPECTRUM_MODE_FINE;

		for (int i = 1; i < argc; i++) {
			run(argv[i], mode);
		}
		report(m ? "wide" : "fine");
	}

	/* Wide layout, per band: median and p90 in log2_q4 units - shows
	 * whether the top (hi-hat) bands actually carry visible energy.
	 */
	printf("wide per band (tilted log2_q4 p50/p90):\n");
	for (int b = 0; b < SPECTRUM_WIDE_BANDS; b++) {
		qsort(band_vals[b], band_n[b], sizeof(int32_t), cmp_int32);
		int32_t p50 = band_vals[b][band_n[b] / 2];
		int32_t p90 = band_vals[b][band_n[b] * 9 / 10];

		printf("  %2d %5u-%5uHz  %3d/%3d\n", b,
		       (unsigned)(fft_wide_lo[b] * 16000u / FFT_N),
		       (unsigned)((fft_wide_hi[b] + 1) * 16000u / FFT_N),
		       p50, p90);
	}
	return 0;
}
