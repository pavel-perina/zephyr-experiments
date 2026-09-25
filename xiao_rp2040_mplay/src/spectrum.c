#include "spectrum.h"

/* Q(23).8 for filter state (low/band) and the input samples promoted into
 * that scale - same FILTER_FRAC_BITS=8 convention as mod_player.c's own
 * one-pole filter. Coefficients (spectrum_f_q14/spectrum_q_q14) are Q0.14,
 * matching mod_player.c's ALPHA_FRAC_BITS=14 for filter_alpha.
 */
#define STATE_FRAC_BITS 8
#define COEFF_FRAC_BITS 14

/* Chamberlin state-variable filter, one instance per band, run over every
 * sample - see mod_player.c's own one-pole filter for the same fixed-point
 * multiply-accumulate-then-shift pattern this follows.
 */
static int32_t low_state[SPECTRUM_BANDS];
static int32_t band_state[SPECTRUM_BANDS];

/* Smoothed per-band peak, persistent across calls - same fast-attack/
 * slow-decay ballistics idea as vu_neopixel.c's envelope, just fixed-point
 * instead of float since this runs per-sample-derived-per-buffer across 16
 * bands rather than once per buffer for a single value.
 */
static int32_t envelope[SPECTRUM_BANDS];

#define ATTACK_ALPHA_Q14 14746   /* ~0.9 in Q0.14 */
#define DECAY_ALPHA_Q14  819     /* ~0.05 in Q0.14 */

/* Calibrated empirically (host harness, same pattern as the VU meter's
 * LEVEL_GAIN) against AXEL_F.MOD's real output after fixing the int32
 * overflow above: raw envelope values across all 16 bands peaked around
 * 2-13 million (Q23.8) with typical averages 0.9-3.8 million. 8M lets the
 * loudest bands' peaks reach full bar height without every band clipping
 * there constantly.
 */
#define ENVELOPE_MAX 8000000

void spectrum_update(const int16_t *samples, size_t n)
{
	int32_t buf_peak[SPECTRUM_BANDS] = {0};

	for (size_t s = 0; s < n; s++) {
		int32_t input = (int32_t)samples[s] << STATE_FRAC_BITS;

		for (int i = 0; i < SPECTRUM_BANDS; i++) {
			/* coefficient (Q0.14, up to ~16384) * state (Q23.8, can
			 * legitimately reach the millions at a resonant peak)
			 * overflows int32_t before the shift - promote to
			 * int64_t for the multiply, same as any fixed-point
			 * product whose operands' ranges can multiply past 2^31.
			 * Cortex-M0 has SMULL (32x32->64) as a baseline ARMv6-M
			 * instruction, so this is one real instruction, not a
			 * software 64-bit multiply routine.
			 */
			low_state[i] += (int32_t)(((int64_t)spectrum_f_q14[i] * band_state[i]) >> COEFF_FRAC_BITS);

			int32_t resonance = (int32_t)(((int64_t)spectrum_q_q14[i] * band_state[i]) >> COEFF_FRAC_BITS);
			int32_t high = input - low_state[i] - resonance;

			band_state[i] += (int32_t)(((int64_t)spectrum_f_q14[i] * high) >> COEFF_FRAC_BITS);

			int32_t abs_band = band_state[i] < 0 ? -band_state[i] : band_state[i];

			if (abs_band > buf_peak[i]) {
				buf_peak[i] = abs_band;
			}
		}
	}

	for (int i = 0; i < SPECTRUM_BANDS; i++) {
		int32_t alpha = (buf_peak[i] > envelope[i]) ? ATTACK_ALPHA_Q14 : DECAY_ALPHA_Q14;

		envelope[i] += (int32_t)(((int64_t)alpha * (buf_peak[i] - envelope[i])) >> COEFF_FRAC_BITS);
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
