#include <string.h>

#include "spectrum.h"
#include "fft_coeffs.h"

_Static_assert(FFT_BANDS == SPECTRUM_BANDS, "regenerate fft_coeffs.h: --bands must match SPECTRUM_BANDS");
_Static_assert(FFT_WIDE_BANDS == SPECTRUM_WIDE_BANDS,
	       "regenerate fft_coeffs.h: --wide-bands must match SPECTRUM_WIDE_BANDS");

/* 512-point version of the earlier 64-point-every-buffer FFT (see git
 * history for that one, and the 16-band IIR bank before it). A bigger FFT
 * run less often is NOT fewer total multiplies - N*log(N) grows faster
 * than N, so 512-point once per ~6 buffers is actually *more* total work
 * than 64-point every buffer. The real point is moving that work off the
 * render thread entirely: spectrum_accumulate() (cheap - just copying
 * ~85 decimated samples into a ring buffer) stays inline in fill_buffer(),
 * same as vu_neopixel_update(); spectrum_process() (the FFT itself, plus
 * the OLED push) runs in its own lower-priority thread (see main.c),
 * where Zephyr's preemptive scheduler guarantees the render thread always
 * wins whenever it has work, regardless of how long this thread's
 * blocking I2C write takes - so the OLED push no longer needs the
 * page-chunking hack either (see oled_display.c).
 *
 * Classic Q1.15 scaled radix-2 decimation-in-time FFT - the standard
 * fixed-point DSP approach (same technique CMSIS-DSP's arm_cfft_q15
 * uses): every butterfly's outputs are halved (>>1) before being stored,
 * which exactly cancels the worst-case 2x magnitude growth a butterfly
 * addition can produce, so after FFT_STAGES stages nothing has grown
 * unboundedly - no int64 promotion needed anywhere in the FFT itself,
 * unlike the IIR bank's resonant-peak growth.
 */

#define DECIMATE 3   /* input samples per FFT sample, 48000/3 = 16000Hz - matches fft_coeffs.h's --fs */

/* Ring buffer of the most recent FFT_N decimated samples (~6 audio buffers'
 * worth at 85 decimated samples/buffer). Written continuously by
 * spectrum_accumulate() (render thread), read wholesale by
 * spectrum_process() (background thread) - no locking: this is display
 * data, not correctness-critical, brief tearing is a non-issue (same
 * relaxed treatment as e.g. player.current_position read by a report
 * loop elsewhere in this project).
 */
static int16_t ring[FFT_N];
static size_t ring_pos;

static int16_t fft_re[FFT_N];
static int16_t fft_im[FFT_N];

static int32_t envelope[SPECTRUM_BANDS];   /* sized for the larger (fine) layout */

/* Requested mode (settable from any thread, see spectrum_set_mode()) vs the
 * one envelope[] currently holds - spectrum_process() resets the envelopes
 * on a switch, since the two layouts' bands don't correspond.
 */
static volatile enum spectrum_mode requested_mode = SPECTRUM_DEFAULT_MODE;
static enum spectrum_mode active_mode = SPECTRUM_DEFAULT_MODE;

/* Wide mode's falling peak dots, Q8.8 pixels (so they can fall by
 * fractions of a pixel per frame).
 */
static int32_t peak_q8[SPECTRUM_WIDE_BANDS];

/* These were tuned for the old cadence - envelope smoothed once per audio
 * buffer, ~188Hz (5.33ms/step) - and left unchanged when spectrum_process()
 * moved to its own thread on a 40ms timer (~25Hz, ~7.5x less often). Same
 * per-step multiplier applied far less often in real time reads as a much
 * slower decay - reported as "really slow fall". Recomputed to preserve
 * the *same* real-time time constants at the new update rate (alpha = 1 -
 * exp(-T/tau), solved for tau from the old alpha/T, then back out the new
 * alpha at T=40ms): tau_decay implied by the old value was ~104ms, giving
 * DECAY_ALPHA_Q14=5243 here; tau_attack was ~2.3ms, which is now faster
 * than the update period itself can show any smoothing on anyway, so
 * attack is just an instant snap-to-new-peak (alpha=1.0).
 */
#define ATTACK_ALPHA_Q14 16384   /* 1.0 in Q0.14 - instant attack */
#define DECAY_ALPHA_Q14  5243    /* ~0.32 in Q0.14 */

/* Calibrated empirically (host harness against AXEL_F.MOD): the 512-point
 * FFT's |re|+|im| envelope peaked around 700-8300 across bands, mostly
 * clustered under ~3000. 5000 gives a reasonable spread without the
 * loudest band (typically the low-mid bass/kick range) clipping too much
 * of the time.
 */
#define ENVELOPE_MAX 5000

/* Wide mode's log (dB) amplitude scale, in log2_q4() units (1/16 of an
 * octave of amplitude = ~0.38dB), applied after the tilt below:
 * LOG_TOP_Q4 maps to a full-height bar, LOG_RANGE_Q4 below that to an
 * empty one. Calibrated with tools/spectrum_calibrate.c over all nine
 * songs in mods/ (tilted values, every band and 40ms frame): p10=127,
 * p50=160, p90=184, p99=201, p99.9=214. So the top sits between p99 and
 * p99.9 (loud bands touch it now and then, Winamp-style bouncing) and a
 * ~30dB range puts the floor right at p10 (quiet passages drop to empty).
 */
#ifndef LOG_TOP_Q4
#define LOG_TOP_Q4   205
#endif
#ifndef LOG_RANGE_Q4
#define LOG_RANGE_Q4 80    /* ~30dB: 80 / 16 octaves * 6.02dB */
#endif

/* Wide mode's tilt compensation, in log2_q4() units per octave above the
 * lowest band (8 = half an octave of amplitude = ~3dB/octave). Music's
 * energy falls roughly 3-6dB/octave (plus mod_player's 4.5kHz Amiga
 * low-pass on top), so without this the highest bands - where hi-hats live
 * - barely leave the floor on a plain log scale. 0 disables it.
 */
#ifndef TILT_Q4_PER_OCT
#define TILT_Q4_PER_OCT 8
#endif

/* Peak dot fall speed, Q8.8 pixels per frame (~25 frames/s). */
#define PEAK_FALL_Q8 (256 * 3 / 4)

void spectrum_accumulate(const int16_t *samples, size_t n)
{
	for (size_t idx = 0; idx < n; idx += DECIMATE) {
		ring[ring_pos] = samples[idx];
		ring_pos = (ring_pos + 1) % FFT_N;
	}
}

static void fft_q15(void)
{
	/* Circular-shift-invariant: a shifted start point in the ring only
	 * adds a linear phase ramp in the frequency domain, which the
	 * |re|+|im| magnitude in spectrum_process() below is blind to
	 * anyway - so copying in raw storage order (no unwrapping to time
	 * order) is exactly as correct as unwrapping would be, for this
	 * magnitude-only use, and cheaper.
	 */
	for (int i = 0; i < FFT_N; i++) {
		fft_re[i] = ring[i];
		fft_im[i] = 0;
	}

	for (int i = 0; i < FFT_N; i++) {
		int j = fft_bitrev[i];

		if (j > i) {
			int16_t t = fft_re[i];

			fft_re[i] = fft_re[j];
			fft_re[j] = t;
			t = fft_im[i];
			fft_im[i] = fft_im[j];
			fft_im[j] = t;
		}
	}

	int half = 1;

	for (int stage = 0; stage < FFT_STAGES; stage++) {
		int step = FFT_N / (half * 2);   /* twiddle table stride for this stage */

		for (int group = 0; group < FFT_N; group += half * 2) {
			for (int k = 0; k < half; k++) {
				int ie = group + k;
				int io = ie + half;
				int16_t wr = fft_twiddle_re[k * step];
				int16_t wi = fft_twiddle_im[k * step];
				int16_t or_ = fft_re[io];
				int16_t oi = fft_im[io];

				/* complex multiply (odd term * twiddle), Q1.15 x Q1.15
				 * -> Q2.30 in the product, >>15 back to Q1.15.
				 */
				int32_t tr = (((int32_t)or_ * wr) - ((int32_t)oi * wi)) >> 15;
				int32_t ti = (((int32_t)or_ * wi) + ((int32_t)oi * wr)) >> 15;
				int32_t er = fft_re[ie];
				int32_t ei = fft_im[ie];

				fft_re[ie] = (int16_t)((er + tr) >> 1);
				fft_im[ie] = (int16_t)((ei + ti) >> 1);
				fft_re[io] = (int16_t)((er - tr) >> 1);
				fft_im[io] = (int16_t)((ei - ti) >> 1);
			}
		}
		half *= 2;
	}
}

/* |re|+|im|: cheap magnitude approximation - no sqrt. */
static int32_t bin_mag(int bin)
{
	int32_t re = fft_re[bin];
	int32_t im = fft_im[bin];

	return (re < 0 ? -re : re) + (im < 0 ? -im : im);
}

void spectrum_set_mode(enum spectrum_mode mode)
{
	requested_mode = mode;
}

const char *spectrum_mode_name(enum spectrum_mode mode)
{
	switch (mode) {
	case SPECTRUM_MODE_FINE:
		return "fine";
	case SPECTRUM_MODE_WIDE:
		return "wide";
	case SPECTRUM_MODE_SCOPE:
		return "scope";
	default:
		return "?";
	}
}

enum spectrum_mode spectrum_get_mode(void)
{
	return requested_mode;
}

int spectrum_band_count(void)
{
	return (active_mode == SPECTRUM_MODE_WIDE) ? SPECTRUM_WIDE_BANDS : SPECTRUM_BANDS;
}

enum spectrum_mode spectrum_process(void)
{
	enum spectrum_mode mode = requested_mode;

	if (mode != active_mode) {
		memset(envelope, 0, sizeof(envelope));
		memset(peak_q8, 0, sizeof(peak_q8));
		active_mode = mode;
	}

	if (mode == SPECTRUM_MODE_SCOPE) {
		return mode;   /* scope reads the ring directly - no FFT needed */
	}

	fft_q15();

	int n = spectrum_band_count();

	for (int i = 0; i < n; i++) {
		int32_t mag;

		if (mode == SPECTRUM_MODE_WIDE) {
			/* Max over the band's whole bin range, not one bin:
			 * noise-like content (hi-hats) is spread across many
			 * bins, so any single one catches only a fraction.
			 */
			mag = 0;
			for (int bin = fft_wide_lo[i]; bin <= fft_wide_hi[i]; bin++) {
				int32_t m = bin_mag(bin);

				if (m > mag) {
					mag = m;
				}
			}
		} else {
			mag = bin_mag(fft_band_bin[i]);
		}

		int32_t alpha = (mag > envelope[i]) ? ATTACK_ALPHA_Q14 : DECAY_ALPHA_Q14;

		envelope[i] += (int32_t)(((int64_t)alpha * (mag - envelope[i])) >> 14);
	}

	return mode;
}

void spectrum_get_raw_envelope(int32_t *out)
{
	int n = spectrum_band_count();

	for (int i = 0; i < n; i++) {
		out[i] = envelope[i];
	}
}

/* log2(v) in Q4 (16 steps per octave): integer part from the top set bit,
 * fraction from the next 4 bits below it - linear interpolation of the
 * mantissa, within ~0.09 octave of the true log2. Cheap on an M0+ (no FPU,
 * no CLZ instruction either - the loop is at most 31 steps).
 */
static int32_t log2_q4(uint32_t v)
{
	if (v == 0) {
		return 0;
	}
	int msb = 31;

	while (!(v & (1u << msb))) {
		msb--;
	}
	uint32_t frac = (msb >= 4) ? (v >> (msb - 4)) : (v << (4 - msb));

	return msb * 16 + (int32_t)(frac & 15);
}

/* Wide band i's displayed level on the log scale, before mapping to
 * pixels: log2 of its envelope plus the tilt for its octave.
 */
static int32_t wide_log_q4(int i)
{
	return log2_q4((uint32_t)envelope[i]) +
	       (((int32_t)fft_wide_oct_q8[i] * TILT_Q4_PER_OCT) >> 8);
}

static uint8_t clamp_level(int32_t v, uint8_t max_value)
{
	if (v > max_value) {
		return max_value;
	}
	return (v < 0) ? 0 : (uint8_t)v;
}

void spectrum_get_levels(uint8_t *out, uint8_t *peaks, uint8_t max_value)
{
	int n = spectrum_band_count();

	for (int i = 0; i < n; i++) {
		if (active_mode != SPECTRUM_MODE_WIDE) {
			out[i] = clamp_level((envelope[i] * (int32_t)max_value) / ENVELOPE_MAX,
					     max_value);
			if (peaks) {
				peaks[i] = 0;   /* fine mode: no peak dots */
			}
			continue;
		}

		int32_t db = wide_log_q4(i) - (LOG_TOP_Q4 - LOG_RANGE_Q4);
		uint8_t level = clamp_level((db * max_value) / LOG_RANGE_Q4, max_value);

		out[i] = level;

		/* Winamp-style peak dot: jumps up with the bar, then falls at a
		 * constant speed until the bar catches it again.
		 */
		int32_t level_q8 = (int32_t)level << 8;

		if (level_q8 >= peak_q8[i]) {
			peak_q8[i] = level_q8;
		} else {
			peak_q8[i] -= PEAK_FALL_Q8;
			if (peak_q8[i] < level_q8) {
				peak_q8[i] = level_q8;
			}
		}
		if (peaks) {
			peaks[i] = (uint8_t)(peak_q8[i] >> 8);
		}
	}
}

/* Scope: 2 decimated (16kHz) samples per pixel column -> a 128px trace
 * spans 256 samples = 16ms. SCOPE_GAIN maps +-32768/SCOPE_GAIN to the full
 * half-height: measured over mods/ (tools/, same host build as the
 * spectrum calibration), |sample| p99 = 14.7k and p99.9 = 20.3k, so gain 2
 * (+-16384 = full height) fills the screen with only the loudest peaks
 * clipped at the edge.
 */
#define SCOPE_STEP 2
#define SCOPE_GAIN 2

void spectrum_get_scope(uint8_t *ys, int width, int height)
{
	/* Snapshot the ring oldest -> newest. The render thread keeps
	 * writing meanwhile, so the snapshot may tear by a few samples -
	 * harmless for a display, same relaxed rule as the FFT's copy.
	 */
	static int16_t snap[FFT_N];
	size_t pos = ring_pos;

	for (int k = 0; k < FFT_N; k++) {
		snap[k] = ring[(pos + k) % FFT_N];
	}

	int span = width * SCOPE_STEP;

	if (span > FFT_N) {
		span = FFT_N;
	}

	/* Latest rising zero crossing with a full window after it. */
	int start = FFT_N - span;

	for (int i = FFT_N - span; i > 0; i--) {
		if (snap[i - 1] < 0 && snap[i] >= 0) {
			start = i;
			break;
		}
	}

	int half = height / 2;

	for (int x = 0; x < width; x++) {
		int idx = start + x * SCOPE_STEP;
		int32_t s = (idx < FFT_N) ? snap[idx] : 0;
		int32_t y = half - (s * SCOPE_GAIN * half) / 32768;

		if (y < 0) {
			y = 0;
		} else if (y > height - 1) {
			y = height - 1;
		}
		ys[x] = (uint8_t)y;
	}
}
