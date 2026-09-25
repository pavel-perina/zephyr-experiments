#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stddef.h>
#include <stdint.h>

#define SPECTRUM_BANDS      128   /* fine layout; must match fft_coeffs.h's --bands */
#define SPECTRUM_WIDE_BANDS 32    /* wide layout; must match fft_coeffs.h's --wide-bands */
#define SPECTRUM_MAX_BANDS  SPECTRUM_BANDS   /* size for per-band output arrays */

/* Two display layouts over the same 512-point FFT:
 *  - FINE: 128 one-pixel bars, each a single nearest FFT bin, linear
 *    amplitude. Lots of low-end detail; noisy, sparse highs.
 *  - WIDE: Winamp-style - 32 bars (3px + 1px gap) with log-spaced edges,
 *    each the max over its whole bin range, log (dB) amplitude and falling
 *    peak dots. Hi-hats/cymbals actually show up.
 */
enum spectrum_mode {
	SPECTRUM_MODE_FINE,
	SPECTRUM_MODE_WIDE,
};

/* Build-time default - override with e.g. -DSPECTRUM_DEFAULT_MODE=SPECTRUM_MODE_FINE,
 * or switch at runtime with spectrum_set_mode().
 */
#ifndef SPECTRUM_DEFAULT_MODE
#define SPECTRUM_DEFAULT_MODE SPECTRUM_MODE_WIDE
#endif

/* Safe to call from any thread: takes effect at the next
 * spectrum_process(), which resets the envelopes/peaks for the new layout.
 */
void spectrum_set_mode(enum spectrum_mode mode);
enum spectrum_mode spectrum_get_mode(void);

/* Bars in the layout spectrum_process() last ran (SPECTRUM_BANDS or
 * SPECTRUM_WIDE_BANDS) - how many entries spectrum_get_levels() fills.
 */
int spectrum_band_count(void);

/* Call once per rendered audio buffer (same input as vu_neopixel_update() -
 * the mixer's raw mono PCM, before any buzzer-only gain/clamp), from the
 * render thread. Cheap: just decimates and copies samples into a ring
 * buffer, no FFT here - see spectrum_process().
 */
void spectrum_accumulate(const int16_t *samples, size_t n);

/* Runs the fixed-point Q1.15 FFT (see spectrum.c) over the ring buffer's
 * current contents and updates each band's smoothed peak envelope from the
 * binned magnitude - fixed-point throughout (RP2040/Cortex-M0 has no FPU,
 * same reasoning as mod_player.c's mixer). Not cheap enough for the render
 * thread - call this from its own lower-priority thread instead (see
 * main.c), on whatever cadence makes sense for how often the display
 * should actually update.
 */
void spectrum_process(void);

/* Fills out[0..spectrum_band_count()-1] with each band's current envelope,
 * scaled to 0..max_value (inclusive) - e.g. max_value = OLED bar height in
 * pixels. If `peaks` is non-NULL it gets the same count of peak-dot heights
 * (0 = none; always 0 in fine mode). Call once per displayed frame: it also
 * advances the peak dots' fall.
 */
void spectrum_get_levels(uint8_t *out, uint8_t *peaks, uint8_t max_value);

/* Raw, unscaled per-band envelope values (roughly int16-ish range - the
 * FFT's |re|+|im| magnitude approximation, smoothed) - diagnostic/
 * calibration use (see tools/ for how ENVELOPE_MAX in spectrum.c was
 * picked), not needed for normal display use.
 */
void spectrum_get_raw_envelope(int32_t *out);

#endif
