#include "spectrum.h"
#include "fft_coeffs.h"

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

static int32_t envelope[SPECTRUM_BANDS];

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

void spectrum_process(void)
{
	fft_q15();

	for (int i = 0; i < SPECTRUM_BANDS; i++) {
		int bin = fft_band_bin[i];
		int16_t re = fft_re[bin];
		int16_t im = fft_im[bin];
		int32_t mag = (re < 0 ? -re : re) + (im < 0 ? -im : im);   /* |re|+|im|, cheap magnitude approximation - no sqrt */
		int32_t alpha = (mag > envelope[i]) ? ATTACK_ALPHA_Q14 : DECAY_ALPHA_Q14;

		envelope[i] += (int32_t)(((int64_t)alpha * (mag - envelope[i])) >> 14);
	}
}

void spectrum_get_raw_envelope(int32_t *out)
{
	for (int i = 0; i < SPECTRUM_BANDS; i++) {
		out[i] = envelope[i];
	}
}

void spectrum_get_levels(uint8_t *out, uint8_t max_value)
{
	for (int i = 0; i < SPECTRUM_BANDS; i++) {
		int32_t scaled = (envelope[i] * (int32_t)max_value) / ENVELOPE_MAX;

		if (scaled > max_value) {
			scaled = max_value;
		} else if (scaled < 0) {
			scaled = 0;
		}
		out[i] = (uint8_t)scaled;
	}
}
