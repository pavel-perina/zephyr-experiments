/*
 * Internal interface between audio.c (shared buffering) and a per-SoC
 * backend. Not for application code - use audio.h.
 *
 * Buffer sequencing (same scheme as pico_mplay's main.c): buffers are
 * numbered by a running sequence number, buffer n lives in audio_buf[n & 1].
 * `consumed` is the number of buffers that have finished playing - so
 * buffer `consumed` is the one playing now. The backend reports each
 * finished buffer by calling audio_core_advance() from its interrupt; the
 * render thread fills buffer `filled` and bumps `filled`. Buffer n must be
 * filled (filled > n) by the time it starts playing, else it's an underrun.
 */
#ifndef AUDIO_BACKEND_H
#define AUDIO_BACKEND_H

#include <stdint.h>

#include "audio.h"

/* Duty-cycle values, 0..top as returned by audio_hw_init(). Must stay in
 * RAM (DMA / EasyDMA source).
 */
extern uint16_t audio_buf[2][AUDIO_BUF_LEN];

/* Counters the backend updates (irqs, errors, last_error, restarts). */
extern volatile struct audio_stats audio_stats;

/* --- implemented by the backend --- */

/* Sets up the hardware; returns 0 and the PWM top value (duty range
 * 0..*top) plus the actual sample rate.
 */
int audio_hw_init(uint32_t *top, uint32_t *sample_rate);

/* Starts playing audio_buf[0], then audio_buf[1], alternating forever,
 * calling audio_core_advance() at the end of each.
 */
int audio_hw_start(void);

/* Called from the render loop, e.g. for a stall watchdog. May be empty. */
void audio_hw_poll(void);

/* --- implemented by audio.c, for the backend --- */

/* Sequence number of the buffer playing now (= buffers finished so far). */
uint32_t audio_core_consumed(void);

/* A buffer finished playing: the next one (consumed + 1) is starting.
 * Counts an underrun if it wasn't filled yet, advances `consumed`, and
 * wakes the render thread. Call from the backend's interrupt (or with
 * interrupts locked).
 */
void audio_core_advance(void);

#endif
