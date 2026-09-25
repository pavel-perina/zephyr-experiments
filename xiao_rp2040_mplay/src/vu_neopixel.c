#include <math.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>

#include "vu_neopixel.h"

static const struct device *strip_dev;

/* Real VU meters are RMS-based (averaging/ballistic), not peak - a "PPM
 * peak meter" is the other, faster-reacting kind. Peak-per-buffer pinned
 * this to full green almost constantly, since any single transient above
 * half-scale (common - percussion attacks, sample clicks) maxed it out;
 * RMS is always lower than peak for the same signal (typical music has a
 * few dB to several dB of crest factor), so it naturally spends less time
 * pinned and reads closer to perceived loudness.
 */
#define MAX_BRIGHTNESS 160   /* out of 255 - full white LED at 5cm is a lot */

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

/* Green -> yellow -> red by level, single pixel standing in for a full VU
 * bar graph's colour range. Breakpoints are arbitrary/by-eye, not measured
 * against anything - easy to retune once it's actually visible.
 */
static struct led_rgb colour_for(float level)
{
	struct led_rgb c = {0};
	float b = MAX_BRIGHTNESS;

	if (level < 0.5f) {
		c.g = (uint8_t)(b * (level / 0.5f));
	} else if (level < 0.8f) {
		c.g = (uint8_t)b;
		c.r = (uint8_t)(b * ((level - 0.5f) / 0.3f));
	} else {
		c.r = (uint8_t)b;
		float t = (level - 0.8f) / 0.2f;

		if (t > 1.0f) {
			t = 1.0f;
		}
		c.g = (uint8_t)(b * (1.0f - t));
	}
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
	float level = rms / 32768.0f;
	float alpha = (level > envelope) ? ATTACK_ALPHA : DECAY_ALPHA;

	envelope += alpha * (level - envelope);

	struct led_rgb pixel = colour_for(envelope);

	led_strip_update_rgb(strip_dev, &pixel, 1);
}
