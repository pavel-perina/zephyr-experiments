#include <math.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>

#include "vu_neopixel.h"
#include "color_luts.h"

static const struct device *strip_dev;

/* The generated LUT (tools/color_gradient.py) is full-fidelity/vivid - the
 * green/red visual-brightness imbalance and any hue correction already
 * live in the OKLCH interpolation that built it, not here. This is purely
 * "how much current the NeoPixel draws" control: reported as making
 * playback less stable at higher levels (plausibly the LED's own current
 * draw sagging the supply enough to disturb the MCU) and "really
 * blinding" at close range regardless. A uniform per-channel cap scales
 * the LUT's output down without shifting hue or the relative ratios
 * between channels, unlike clamping each channel independently would.
 */
#define MAX_CHANNEL 16

/* Real VU meters are RMS-based (averaging/ballistic), not peak - a "PPM
 * peak meter" is the other, faster-reacting kind. Peak-per-buffer pinned
 * this to full green almost constantly, since any single transient above
 * half-scale (common - percussion attacks, sample clicks) maxed it out;
 * RMS is always lower than peak for the same signal (typical music has a
 * few dB to several dB of crest factor), so it naturally spends less time
 * pinned and reads closer to perceived loudness.
 */

/* Checked empirically (tools/mod_to_wav-style host harness) against
 * AXEL_F.MOD: the RMS envelope never exceeds ~33% even at the song's
 * loudest moments - MIX_SCALE keeps real headroom for the I2S DAC/amp
 * chain (see pico_dma/README.md), so 0..1 raw was never reachable. This
 * rescales so the loudest realistic content actually reaches the top of
 * the LUT (red) instead of topping out a third of the way through it.
 */
#define LEVEL_GAIN 3.0f

/* VU-style ballistics: attack (level rising) is fast so the LED tracks
 * transients, decay (level falling) is slow so it reads like a needle
 * settling rather than flickering every buffer (~5.33ms at 48kHz/256
 * samples - way faster than anything eye-readable). Plain float is fine
 * here: this runs once per buffer in the render thread, not per sample.
 */
#define ATTACK_ALPHA 0.9f
#define DECAY_ALPHA  0.05f

static float envelope;   /* 0..1 */

int vu_neopixel_init(void)
{
	strip_dev = DEVICE_DT_GET(DT_ALIAS(led_strip));
	if (!device_is_ready(strip_dev)) {
		strip_dev = NULL;
		return -1;
	}
	return 0;
}

static struct led_rgb colour_for(float level)
{
	int idx = (int)(level * (COLOR_LUT_SIZE - 1));

	if (idx < 0) {
		idx = 0;
	} else if (idx >= COLOR_LUT_SIZE) {
		idx = COLOR_LUT_SIZE - 1;
	}

	const uint8_t *rgb = blue_pink_white_lut[idx];
	struct led_rgb c = {
		.r = (uint8_t)((uint32_t)rgb[0] * MAX_CHANNEL / 255),
		.g = (uint8_t)((uint32_t)rgb[1] * MAX_CHANNEL / 255),
		.b = (uint8_t)((uint32_t)rgb[2] * MAX_CHANNEL / 255),
	};
	return c;
}

void vu_neopixel_update(const int16_t *samples, size_t n)
{
	if (!strip_dev) {
		return;
	}

	uint64_t sum_sq = 0;

	for (size_t i = 0; i < n; i++) {
		int32_t v = samples[i];

		sum_sq += (uint32_t)(v * v);
	}

	float rms = sqrtf((float)sum_sq / (float)n);
	float level = (rms / 32768.0f) * LEVEL_GAIN;

	if (level > 1.0f) {
		level = 1.0f;
	}
	float alpha = (level > envelope) ? ATTACK_ALPHA : DECAY_ALPHA;

	envelope += alpha * (level - envelope);

	struct led_rgb pixel = colour_for(envelope);

	led_strip_update_rgb(strip_dev, &pixel, 1);
}
