/*
 * Audio output: a double-buffered stream of PCM samples to the buzzer.
 *
 * Board-independent interface. The shared part (audio.c) owns the two
 * buffers and the consumed/filled sequence counters; a per-SoC backend
 * (audio_rp2xxx.c: PWM + DMA, audio_nrf.c: nRF PWM sequences) moves the
 * buffers to the hardware - see audio_backend.h.
 *
 * Usage, from one (render) thread:
 *
 *   audio_init();
 *   while (audio_wants_buffer()) { render pcm; audio_submit(pcm); }   // prefill
 *   audio_start();
 *   loop {
 *       audio_wait(timeout);
 *       audio_poll();
 *       while (audio_wants_buffer()) { render pcm; audio_submit(pcm); }
 *   }
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#define AUDIO_BUF_LEN 256   /* samples per buffer, ~5.3ms at 48kHz */

struct audio_stats {
	uint32_t irqs;       /* buffer-done interrupts */
	uint32_t underruns;  /* a buffer started playing before it was refilled */
	uint32_t errors;     /* backend-reported transfer errors */
	int last_error;
	uint32_t restarts;   /* stall-watchdog recoveries (RP2xxx only) */
};

/* Configures the output hardware; doesn't start playback. 0 on success. */
int audio_init(void);

/* Actual output sample rate in Hz (the PWM period can't always hit 48000
 * exactly - e.g. 48048 on nRF52).
 */
uint32_t audio_sample_rate(void);

/* True while a buffer is free to be filled with audio_submit(). */
bool audio_wants_buffer(void);

/* Converts AUDIO_BUF_LEN signed 16-bit samples into the next free buffer
 * and queues it. Only call when audio_wants_buffer().
 */
void audio_submit(const int16_t *pcm);

/* Starts playback of the queued buffers - call after the prefill. */
int audio_start(void);

/* Blocks until a buffer has been played (so one is free), or timeout. */
void audio_wait(k_timeout_t timeout);

/* Backend housekeeping from the render loop (RP2xxx: DMA stall watchdog). */
void audio_poll(void);

void audio_get_stats(struct audio_stats *out);

#endif
