/*
 * STM32 audio backend (written for the NUCLEO-L552ZE-Q): a timer's PWM
 * channel drives the buzzer, its period is one sample (48 kHz), and DMA -
 * paced by the timer's update event, i.e. once per period - writes the
 * next duty value into the channel's compare register. Same idea as
 * audio_rp2xxx.c (PWM wrap paces the DMA), different plumbing:
 *
 * - Zephyr's PWM driver sets up the timer: clock, pin mux (pinctrl in the
 *   board overlay), period and PWM mode, with ARR and CCR preload on - so
 *   a value the DMA writes into CCRx only takes effect at the next update,
 *   never mid-period. The only thing Zephyr's PWM API can't do is ask for
 *   a DMA request per update: that's one register bit (TIMx_DIER.UDE),
 *   set through ST's LL ("low-layer") header, which Zephyr's own STM32
 *   drivers are built on.
 *
 * - Zephyr's STM32 DMA driver does the rest, and unlike the rpi_pico one
 *   it has a working cyclic mode: one circular transfer over both buffers
 *   (audio_buf[0] and [1] are contiguous), with the half-transfer
 *   interrupt reported as DMA_STATUS_BLOCK (buffer 0 finished, buffer 1
 *   playing) and transfer-complete as DMA_STATUS_COMPLETE (buffer 1
 *   finished, wrapping back to buffer 0). That's the ping-pong for free:
 *   nothing to re-arm, so no stall watchdog as in audio_rp2xxx.c.
 *
 * The DMA request line goes through the DMAMUX: request 60 = TIM2_UP on
 * the STM32L5, given in the board overlay's dmas cell.
 */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/dma.h>
#include <soc.h>
#include <stm32_ll_tim.h>

#include "audio_backend.h"

#define SAMPLE_RATE_HZ 48000u

#define USER_NODE DT_PATH(zephyr_user)

static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(USER_NODE);

/* The timer behind the PWM node (the PWM node is a child of timersN). */
static TIM_TypeDef *const tim = (TIM_TypeDef *)DT_REG_ADDR(DT_PARENT(DT_PWMS_CTLR(USER_NODE)));

static const struct device *const dma_dev = DEVICE_DT_GET(DT_DMAS_CTLR(USER_NODE));
#define DMA_CHANNEL DT_DMAS_CELL_BY_IDX(USER_NODE, 0, channel)
#define DMA_REQUEST DT_DMAS_CELL_BY_IDX(USER_NODE, 0, slot)

static void dma_done(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);

	audio_stats.irqs++;
	if (status < 0) {
		/* Count it and keep going, as in the other backends. */
		audio_stats.errors++;
		audio_stats.last_error = status;
		return;
	}
	/* DMA_STATUS_BLOCK (half transfer) or DMA_STATUS_COMPLETE: either
	 * way one buffer has just finished and the other is playing.
	 */
	audio_core_advance();
}

/* Data widths: 16-bit in memory (audio_buf), 32-bit to the peripheral.
 * TIM2's CCRx is a 32-bit register, and on STM32 a 16-bit write to a
 * 32-bit APB register isn't guaranteed to leave the upper half at 0 (the
 * bridge can duplicate the halfword into both halves - CCR = d * 0x10001,
 * way above ARR, i.e. a constant 100% duty and silence). With
 * MSIZE = 16 / PSIZE = 32 the DMA itself zero-extends each sample into a
 * word write.
 *
 * Zephyr's STM32 driver computes the transfer count (NDTR) as
 * block_size / dest_data_size for memory-to-peripheral, so block_size is
 * "number of samples x 4", not the size of audio_buf in bytes.
 */
static struct dma_block_config dma_block = {
	.block_size = 2 * AUDIO_BUF_LEN * 4,
	.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
	.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	.source_reload_en = 1,   /* both reload flags = circular mode */
	.dest_reload_en = 1,
};

static struct dma_config dma_cfg = {
	.dma_slot = DMA_REQUEST,
	.channel_direction = MEMORY_TO_PERIPHERAL,
	.channel_priority = 2,   /* high */
	.source_data_size = 2,
	.dest_data_size = 4,
	.source_burst_length = 1,
	.dest_burst_length = 1,
	.block_count = 1,
	.head_block = &dma_block,
	.dma_callback = dma_done,
};

int audio_hw_init(uint32_t *top, uint32_t *sample_rate)
{
	if (!pwm_is_ready_dt(&buzzer) || !device_is_ready(dma_dev)) {
		printk("audio: pwm or dma device not ready\n");
		return -ENODEV;
	}

	uint64_t cycles_per_sec;
	int err = pwm_get_cycles_per_sec(buzzer.dev, buzzer.channel, &cycles_per_sec);

	if (err != 0) {
		return err;
	}

	/* One PWM period per sample; duty starts at 0 until the DMA runs. */
	uint32_t period_cycles = (uint32_t)(cycles_per_sec / SAMPLE_RATE_HZ);

	err = pwm_set_cycles(buzzer.dev, buzzer.channel, period_cycles, 0, buzzer.flags);
	if (err != 0) {
		printk("audio: pwm_set_cycles failed: %d\n", err);
		return err;
	}

	/* CCR1..CCR4 are consecutive 32-bit registers. */
	dma_block.source_address = (uintptr_t)audio_buf;
	dma_block.dest_address = (uintptr_t)(&tim->CCR1 + (buzzer.channel - 1));

	err = dma_config(dma_dev, DMA_CHANNEL, &dma_cfg);
	if (err != 0) {
		printk("audio: dma_config failed: %d\n", err);
		return err;
	}

	*top = period_cycles - 1;
	*sample_rate = (uint32_t)(cycles_per_sec / period_cycles);
	printk("audio: stm32 TIM @%p channel %u, cycles_per_sec=%llu period_cycles=%u, "
	       "DMA channel %u request %u\n", (void *)tim, buzzer.channel, cycles_per_sec,
	       period_cycles, DMA_CHANNEL, DMA_REQUEST);
	return 0;
}

int audio_hw_start(void)
{
	int err = dma_start(dma_dev, DMA_CHANNEL);

	if (err != 0) {
		printk("audio: dma_start failed: %d\n", err);
		return err;
	}
	/* From now on every timer update requests one DMA transfer. */
	LL_TIM_EnableDMAReq_UPDATE(tim);
	return 0;
}

void audio_hw_poll(void)
{
}
