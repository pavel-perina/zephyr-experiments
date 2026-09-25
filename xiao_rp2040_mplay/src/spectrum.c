#include "spectrum.h"
#include "fft_coeffs.h"

/* Replaces the earlier 16-band IIR filter bank (see git history) after it
 * turned out to cost more total multiplies than a single per-buffer FFT
 * would, for coarser resolution: 16 bands x 3 multiplies x ~85 decimated
 * samples/buffer (~4080/buffer after the 3x decimation experiment) versus
 * this FFT's 64*log2(64)/2 = 192 butterflies x 4 real multiplies = 768
 * multiplies/buffer, done once, covering 33 bins instead of 16 bands.
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

static int16_t fft_re[FFT_N];
static int16_t fft_im[FFT_N];

static int32_t envelope[SPECTRUM_BANDS];

#define ATTACK_ALPHA_Q14 14746   /* ~0.9 in Q0.14 */
#define DECAY_ALPHA_Q14  819     /* ~0.05 in Q0.14 */

/* Calibrated empirically (host harness against AXEL_F.MOD): the |re|+|im|
 * magnitude approximation's envelope peaked around 4800-6900 across bands
 * for this song. 7000 lets the loudest bands reach full bar height without
 * clipping there constantly.
 */
#define ENVELOPE_MAX 7000

static void fft_q15(void)
{
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

void spectrum_update(const int16_t *samples, size_t n)
{
	for (int i = 0; i < FFT_N; i++) {
		size_t idx = (size_t)i * DECIMATE;

		fft_re[i] = (idx < n) ? samples[idx] : 0;
		fft_im[i] = 0;
	}

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
