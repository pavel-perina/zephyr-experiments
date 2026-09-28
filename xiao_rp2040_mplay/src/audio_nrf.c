/*
 * nRF52 audio backend: the PWM peripheral's own sequence playback.
 *
 * Unlike the RP2040 there's no general-purpose DMA controller here - each
 * nRF peripheral has its own built-in DMA ("EasyDMA"), and the PWM one
 * reads a whole sequence of duty values from RAM by itself, one per PWM
 * period. It has two sequence slots, SEQ0 and SEQ1, played back to back and
 * looped - exactly our ping-pong: audio_buf[0] is SEQ0, audio_buf[1] SEQ1.
 * At the end of each, the END_SEQ event says that buffer's data can be
 * rewritten (the other one is playing now) -> audio_core_advance().
 *
 * Driven through nrfx_pwm directly (it ships with Zephyr) because Zephyr's
 * PWM API has no sequence concept. Uses PWM1, which stays disabled in
 * devicetree so Zephyr's own PWM driver never claims it (the board's
 * pwm-led uses PWM0).
 *
 * No stall watchdog as in audio_rp2xxx.c: the peripheral alternates and
 * loops by itself, with nothing to re-arm per buffer - there's no
 * equivalent of the RP2040's double-trigger race.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <soc.h>
#include <nrfx_pwm.h>

#include "audio_backend.h"

/* 16 MHz / 333 = 48048 Hz: the closest a 16 MHz PWM gets to 48 kHz (0.1%
 * sharp - inaudible). Duty values 0..333 are ~8.4 bits of resolution,
 * versus ~11.3 on the RP2040 (125 MHz / 48 kHz) - plenty for a buzzer.
 */
#define PWM_CLOCK_HZ 16000000u
#define PWM_TOP      333u

#define PWM_NODE DT_NODELABEL(pwm1)

/* Buzzer pin from expansion_board.overlay (XIAO connector D3). */
#define BUZZER_PSEL NRF_DT_GPIOS_TO_PSEL(DT_PATH(zephyr_user), buzzer_gpios)

#if defined(CONFIG_BOARD_XIAO_BLE)
BUILD_ASSERT(BUZZER_PSEL == 29, "XIAO BLE D3 should resolve to P0.29");
#endif

static nrfx_pwm_t pwm = NRFX_PWM_INSTANCE(DT_REG_ADDR(PWM_NODE));

static nrf_pwm_sequence_t seq[2];

static void pwm_handler(nrfx_pwm_event_type_t event_type, void *p_context)
{
	ARG_UNUSED(p_context);
	if (event_type == NRFX_PWM_EVENT_END_SEQ0 || event_type == NRFX_PWM_EVENT_END_SEQ1) {
		audio_stats.irqs++;
		audio_core_advance();
	}
}

int audio_hw_init(uint32_t *top, uint32_t *sample_rate)
{
	nrfx_pwm_config_t cfg = {
		.output_pins = {
			BUZZER_PSEL,
			NRF_PWM_PIN_NOT_CONNECTED,
			NRF_PWM_PIN_NOT_CONNECTED,
			NRF_PWM_PIN_NOT_CONNECTED,
		},
		.irq_priority = DT_IRQ(PWM_NODE, priority),
		.base_clock = NRF_PWM_CLK_16MHz,
		.count_mode = NRF_PWM_MODE_UP,
		.top_value = PWM_TOP,
		.load_mode = NRF_PWM_LOAD_COMMON,   /* one value per period, all channels */
		.step_mode = NRF_PWM_STEP_AUTO,     /* next value every period */
	};

	/* Same wiring as Zephyr's own drivers/pwm/pwm_nrfx.c. */
	IRQ_CONNECT(DT_IRQN(PWM_NODE), DT_IRQ(PWM_NODE, priority), nrfx_pwm_irq_handler, &pwm, 0);

	int err = nrfx_pwm_init(&pwm, &cfg, pwm_handler, NULL);

	if (err != 0) {
		printk("audio: nrfx_pwm_init failed: %d\n", err);
		return err;
	}

	for (int i = 0; i < 2; i++) {
		seq[i].values.p_common = audio_buf[i];
		seq[i].length = AUDIO_BUF_LEN;
		seq[i].repeats = 0;     /* each value for exactly one PWM period */
		seq[i].end_delay = 0;
	}

	*top = PWM_TOP;
	*sample_rate = PWM_CLOCK_HZ / PWM_TOP;
	printk("audio: nRF PWM1 on P%d.%02d, top=%u (%u Hz)\n",
	       BUZZER_PSEL >> 5, BUZZER_PSEL & 31, PWM_TOP,
	       *sample_rate);
	return 0;
}

int audio_hw_start(void)
{
	/* SEQ0, SEQ1, SEQ0, ... forever; END_SEQn events drive the refills. */
	nrfx_pwm_complex_playback(&pwm, &seq[0], &seq[1], 1,
				  NRFX_PWM_FLAG_LOOP | NRFX_PWM_FLAG_SIGNAL_END_SEQ0 |
					  NRFX_PWM_FLAG_SIGNAL_END_SEQ1 |
					  NRFX_PWM_FLAG_NO_EVT_FINISHED);
	return 0;
}

void audio_hw_poll(void)
{
}
