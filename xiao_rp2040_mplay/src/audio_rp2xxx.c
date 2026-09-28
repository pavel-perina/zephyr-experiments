/*
 * RP2040 / RP2350 audio backend: the buzzer pin's PWM slice wraps at the
 * sample rate, and DMA - paced by that wrap - writes the next duty value
 * into the slice's compare register each period.
 *
 * Zephyr's pwm.h has no DMA-fed duty-stream concept (one-shot set-period/
 * pulse only), so the PWM driver only sets the period once and DMA writes
 * duty values directly into the raw pwm_hw register. This SoC's DMA driver
 * has no working "repeat forever"/cyclic mode, so the channel is re-armed
 * from its own completion callback - the ISR only re-arms and does the
 * sequence bookkeeping (audio_core_advance()); rendering happens in the
 * render thread (see pico_mplay/src/main.c for why it can't live in the
 * ISR).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/dt-bindings/dma/rpi-pico-dma-common.h>
#include <hardware/gpio.h>
#include <hardware/pwm.h>
#include <hardware/structs/pwm.h>
#include <hardware/structs/dma.h>

#include "audio_backend.h"

/* The buzzer pin, from expansion_board.overlay (XIAO connector D3, resolved to the
 * real GPIO). The PWM slice/channel and the DMA request all follow from
 * it; the pin mux itself is the pinctrl in the board's overlay under
 * boards/ - audio_hw_init() checks the two agree.
 */
#define BUZZER_PIN DT_GPIO_PIN(DT_PATH(zephyr_user), buzzer_gpios)

#if defined(CONFIG_BOARD_XIAO_RP2040)
BUILD_ASSERT(BUZZER_PIN == 29, "XIAO RP2040 D3 should resolve to GPIO29");
#elif defined(CONFIG_BOARD_XIAO_RP2350)
BUILD_ASSERT(BUZZER_PIN == 5, "XIAO RP2350 D3 should resolve to GPIO5");
#endif

static uint slice;      /* pwm_gpio_to_slice_num(BUZZER_PIN) */
static uint channel;    /* pwm_gpio_to_channel(BUZZER_PIN): 0 = A, 1 = B */

#define SAMPLE_RATE_HZ 48000u

static const struct device *pwm_dev = DEVICE_DT_GET(DT_NODELABEL(pwm));
static const struct device *dma_dev = DEVICE_DT_GET(DT_NODELABEL(dma));
static uint32_t dma_channel;
static volatile uint16_t *cc_half;   /* our channel's half of slice[slice].cc */

static void dma_done(const struct device *dev, void *user_data, uint32_t channel, int status);

/* File-scope so dma_done() can re-arm through them (see arm_buffer()). */
static struct dma_block_config dma_block = {
	.dest_address = 0,   /* cc_half, set in audio_hw_init() once it's known */
	.block_size = AUDIO_BUF_LEN * sizeof(uint16_t),
	.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
	.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
};
static struct dma_config dma_cfg = {
	.channel_direction = MEMORY_TO_PERIPHERAL,
	.source_data_size = 2,
	.dest_data_size = 2,
	.source_burst_length = 1,
	.dest_burst_length = 1,
	.block_count = 1,
	.head_block = &dma_block,
	.dma_slot = 0,   /* the slice's wrap DREQ, set in audio_hw_init() */
	.dma_callback = dma_done,
};

/* Points the (idle) channel at buffer `seq & 1` and triggers it - exactly
 * once. This used to be dma_reload() + dma_start(), but on this driver
 * *both* call dma_channel_configure(..., trigger=true): the second one
 * rewrote READ_ADDR/TRANS_COUNT/CTRL_TRIG on a channel that was already
 * running and racing the PWM DREQ. Caught in the act as a permanent stall:
 * BUSY, EN, no error flags, TRANS_COUNT=1, READ_ADDR on the last sample of
 * buf[1], PWM still wrapping - waiting forever for a DREQ that never came.
 * Timing-dependent (IRQ latency vs DREQ phase), hence minutes apart and
 * only with the I2C/OLED interrupt load around. dma_config() only stores
 * the settings in the driver (no register writes), so config + start is a
 * single trigger on an idle channel.
 */
static void arm_buffer(uint32_t seq)
{
	dma_block.source_address = (uintptr_t)audio_buf[seq & 1];
	dma_config(dma_dev, dma_channel, &dma_cfg);
	dma_start(dma_dev, dma_channel);
}

static void dma_done(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);
	audio_stats.irqs++;
	if (status < 0) {
		/* Count it but keep going - returning here without re-arming
		 * kills audio permanently.
		 */
		audio_stats.errors++;
		audio_stats.last_error = status;
	}

	arm_buffer(audio_core_consumed() + 1);
	audio_core_advance();
}

int audio_hw_init(uint32_t *top, uint32_t *sample_rate)
{
	if (!device_is_ready(pwm_dev) || !device_is_ready(dma_dev)) {
		printk("pwm or dma device not ready\n");
		return -ENODEV;
	}

	slice = pwm_gpio_to_slice_num(BUZZER_PIN);
	channel = pwm_gpio_to_channel(BUZZER_PIN);

	/* Zephyr's PWM "channel" numbering is slice*2 + (0=A, 1=B). */
	uint32_t zephyr_channel = slice * 2 + channel;
	uint64_t cycles_per_sec;

	pwm_get_cycles_per_sec(pwm_dev, zephyr_channel, &cycles_per_sec);

	/* PWM slice wraps (and DMA supplies the next duty value) at exactly
	 * SAMPLE_RATE_HZ - same "sample rate via wrap DREQ" concept as
	 * pico_mplay's I2S BCLK derivation, just for a single PWM channel
	 * instead of a bit-clocked serial protocol.
	 */
	uint32_t period_cycles = (uint32_t)(cycles_per_sec / SAMPLE_RATE_HZ);

	pwm_set_cycles(pwm_dev, zephyr_channel, period_cycles, 0, 0);
	cc_half = (volatile uint16_t *)&pwm_hw->slice[slice].cc + channel;
	dma_block.dest_address = (uintptr_t)cc_half;
	dma_cfg.dma_slot = RPI_PICO_DMA_DREQ_TO_SLOT(pwm_get_dreq(slice));

	if (gpio_get_function(BUZZER_PIN) != GPIO_FUNC_PWM) {
		printk("audio: WARNING GPIO%d isn't muxed to PWM - check the pinctrl in "
		       "the board overlay against expansion_board.overlay\n", BUZZER_PIN);
	}

	int ch = dma_request_channel(dma_dev, NULL);

	if (ch < 0) {
		printk("no free dma channel\n");
		return ch;
	}
	dma_channel = (uint32_t)ch;

	*top = period_cycles - 1;
	*sample_rate = (uint32_t)(cycles_per_sec / period_cycles);
	printk("audio: rp2xxx GPIO%d = PWM slice %u channel %c, cycles_per_sec=%llu "
	       "period_cycles=%u\n", BUZZER_PIN, slice, channel ? 'B' : 'A', cycles_per_sec,
	       period_cycles);
	return 0;
}

int audio_hw_start(void)
{
	arm_buffer(0);
	return 0;
}

/* Safety net: a buffer takes ~5.3ms, so 50ms without a single DMA IRQ
 * means the chain is stuck. Dump the raw state (CTRL bit 24 = BUSY, bit 0
 * = EN, bits 29..31 = error flags; PWM CSR bit 0 = slice enabled), abort,
 * and restart from the next buffer as if dma_done() had run. restarts
 * should stay 0 with the arm_buffer() fix - if it doesn't, the stall has
 * another cause, but playback recovers either way.
 */
void audio_hw_poll(void)
{
	static uint32_t watch_irqs;
	static int64_t watch_time;

	if (audio_stats.irqs != watch_irqs) {
		watch_irqs = audio_stats.irqs;
		watch_time = k_uptime_get();
		return;
	}
	if (k_uptime_get() - watch_time <= 50) {
		return;
	}

	printk("  DMA STALL: ctrl=%08x count=%u read=%08x "
	       "inte0=%08x intr=%08x pwm_csr=%08x pwm_ctr=%u - restarting\n",
	       dma_hw->ch[dma_channel].al1_ctrl,
	       dma_hw->ch[dma_channel].transfer_count,
	       dma_hw->ch[dma_channel].read_addr,
	       dma_hw->inte0, dma_hw->intr,
	       pwm_hw->slice[slice].csr,
	       pwm_hw->slice[slice].ctr);

	unsigned int key = irq_lock();

	/* Re-check under the lock: dma_done() may have fired since the check
	 * above, re-arming a healthy transfer that must not be aborted.
	 */
	if (audio_stats.irqs == watch_irqs) {
		dma_stop(dma_dev, dma_channel);
		while (dma_hw->abort & BIT(dma_channel)) {
		}
		arm_buffer(audio_core_consumed() + 1);
		audio_core_advance();
		audio_stats.restarts++;
	}
	irq_unlock(key);
	watch_time = k_uptime_get();
}
