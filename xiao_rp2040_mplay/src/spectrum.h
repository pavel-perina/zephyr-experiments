#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <stddef.h>
#include <stdint.h>

#define SPECTRUM_BANDS 16   /* must match fft_coeffs.h's --bands (fft_band_bin's range) */

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

/* Fills out[SPECTRUM_BANDS] with each band's current envelope, scaled to
 * 0..max_value (inclusive) - e.g. max_value = OLED bar height in pixels.
 */
void spectrum_get_levels(uint8_t *out, uint8_t max_value);

/* Raw, unscaled per-band envelope values (roughly int16-ish range - the
 * FFT's |re|+|im| magnitude approximation, smoothed) - diagnostic/
 * calibration use (see tools/ for how ENVELOPE_MAX in spectrum.c was
 * picked), not needed for normal display use.
 */
void spectrum_get_raw_envelope(int32_t *out);

#endif
