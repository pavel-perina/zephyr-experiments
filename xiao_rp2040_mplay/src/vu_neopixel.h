#ifndef VU_NEOPIXEL_H
#define VU_NEOPIXEL_H

#include <stddef.h>
#include <stdint.h>

/* Grabs the onboard NeoPixel (led-strip alias, chain-length 1) via Zephyr's
 * led_strip API. Returns 0 on success, negative on failure (device not
 * ready) - caller decides whether that's fatal.
 */
int vu_neopixel_init(void);

/* Call once per rendered audio buffer, before the buzzer-specific
 * gain/clamp is applied (i.e. straight off mod_player_produce()'s output) -
 * computes a peak level, applies VU-style attack/decay smoothing across
 * calls, and pushes a colour (green -> yellow -> red by level) to the
 * NeoPixel. Cheap (one pass over the buffer, one LED write), safe to call
 * from the render thread.
 */
void vu_neopixel_update(const int16_t *samples, size_t n);

#endif
