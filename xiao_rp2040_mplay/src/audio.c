/*
 * Shared, board-independent part of the audio output: the two buffers, the
 * consumed/filled sequence counters and PCM -> duty-cycle conversion. The
 * hardware side lives in a backend - see audio_backend.h.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#include "audio.h"
#include "audio_backend.h"

uint16_t audio_buf[2][AUDIO_BUF_LEN];
volatile struct audio_stats audio_stats;

/* consumed: advanced by the backend's interrupt; filled: advanced by the
 * render thread only after a buffer is completely converted. Comparing the
 * two catches every underrun, including the hardware reaching a buffer
 * that's still mid-render (see pico_mplay/src/main.c for why a single
 * flag isn't enough).
 */
static volatile uint32_t consumed;
static volatile uint32_t filled;

K_SEM_DEFINE(audio_sem, 0, 1);

static uint32_t duty_top;
static uint32_t rate_hz;

int audio_init(void)
{
	return audio_hw_init(&duty_top, &rate_hz);
}

uint32_t audio_sample_rate(void)
{
	return rate_hz;
}

bool audio_wants_buffer(void)
{
	return (int32_t)(consumed + 2 - filled) > 0;   /* signed diff: wrap-safe */
}

/* Rescales signed 16-bit PCM into an unsigned duty value in [0, top]. */
void audio_submit(const int16_t *pcm)
{
	uint16_t *b = audio_buf[filled & 1];

	for (int i = 0; i < AUDIO_BUF_LEN; i++) {
		uint32_t shifted = (uint32_t)((int32_t)pcm[i] + 32768);   /* -> [0, 65535] */
		uint32_t duty = (shifted * (duty_top + 1)) >> 16;

		if (duty > duty_top) {
			duty = duty_top;   /* guard the rounding-up edge case at full scale */
		}
		b[i] = (uint16_t)duty;
	}
	barrier_dmem_fence_full();   /* buffer contents visible before `filled` says so */
	filled++;
}

int audio_start(void)
{
	return audio_hw_start();
}

void audio_wait(k_timeout_t timeout)
{
	k_sem_take(&audio_sem, timeout);
}

void audio_poll(void)
{
	audio_hw_poll();
}

void audio_get_stats(struct audio_stats *out)
{
	out->irqs = audio_stats.irqs;
	out->underruns = audio_stats.underruns;
	out->errors = audio_stats.errors;
	out->last_error = audio_stats.last_error;
	out->restarts = audio_stats.restarts;
}

uint32_t audio_core_consumed(void)
{
	return consumed;
}

void audio_core_advance(void)
{
	uint32_t next = consumed + 1;

	if ((int32_t)(filled - next) <= 0) {   /* signed diff: wrap-safe */
		audio_stats.underruns++;
	}
	consumed = next;
	k_sem_give(&audio_sem);
}
