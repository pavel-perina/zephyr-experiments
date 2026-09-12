/*
 * Step 2: DMA + PWM via Zephyr's own driver APIs (not the raw pico-sdk HAL),
 * reproducing pico_mplay's original bare-metal step 1: a DMA channel
 * streams a slow (LED-visible, ~4 Hz) sine wave's duty-cycle values into a
 * PWM slice's compare register, paced by that slice's own wrap DREQ.
 *
 * Zephyr's PWM API has no concept of a DMA-fed duty stream (it's a
 * one-shot "set period+pulse" abstraction, same as most vendor-neutral PWM
 * APIs), so the pattern here is hybrid: Zephyr's pwm.h sets up the slice's
 * period once, then DMA writes duty values directly into the raw hardware
 * register (pwm_hw, from the same pico-sdk HAL header the Zephyr PWM
 * driver itself is built on) on every wrap. Zephyr's dma.h driver has no
 * built-in "repeat forever" mode either (dma_config's `cyclic` flag isn't
 * implemented by this SoC's driver) - like the bare-metal version, this
 * manually re-arms the DMA channel from its own completion callback.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/dma.h>
#if defined(CONFIG_SOC_SERIES_RP2350)
#include <zephyr/dt-bindings/dma/rpi-pico-dma-rp2350.h>
#else
#include <zephyr/dt-bindings/dma/rpi-pico-dma-rp2040.h>
#endif
#include <hardware/structs/pwm.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* GPIO25 = PWM slice 4, channel B on both rpi_pico and rpi_pico2 (see
 * pwm_ch4b_default in boards/common/rpi_pico-pinctrl-common.dtsi).
 * Zephyr's PWM "channel" numbering is slice*2 + (0=A, 1=B).
 */
#define PWM_SLICE   4
#define PWM_CHANNEL (PWM_SLICE * 2 + 1)

#define WAVE_FREQ_HZ 4.0f
#define BUF_LEN      128

static const struct device *pwm_dev;
static const struct device *dma_dev;
static uint32_t dma_channel;
static volatile uint16_t *cc_half;   /* channel B = high half of slice[4].cc */

static uint16_t buf[2][BUF_LEN];
static int playing;
static uint32_t phase;
static uint32_t phase_inc;
static uint32_t pwm_top;

static void fill_buffer(uint16_t *b)
{
	for (int i = 0; i < BUF_LEN; i++) {
		float s = sinf(2.0f * (float)M_PI * (float)phase / 4294967296.0f);
		b[i] = (uint16_t)((s + 1.0f) * 0.5f * (float)pwm_top);
		phase += phase_inc;
	}
}

static void dma_done(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);

	if (status < 0) {
		return;
	}

	/* Simplification vs. the bare-metal version: buffer refill happens
	 * directly in the completion callback instead of being handed off
	 * to a thread via a semaphore/work item. Fine for a small, cheap
	 * sine fill; a real (audio-rate) player would defer this.
	 */
	int next = 1 - playing;
	dma_reload(dma_dev, dma_channel, (uintptr_t)buf[next], (uintptr_t)cc_half,
		   BUF_LEN * sizeof(uint16_t));
	dma_start(dma_dev, dma_channel);
	playing = next;

	fill_buffer(buf[1 - next]);
}

int main(void)
{
	pwm_dev = DEVICE_DT_GET(DT_NODELABEL(pwm));
	dma_dev = DEVICE_DT_GET(DT_NODELABEL(dma));

	if (!device_is_ready(pwm_dev) || !device_is_ready(dma_dev)) {
		printk("pwm or dma device not ready\n");
		return 0;
	}

	uint64_t cycles_per_sec;
	pwm_get_cycles_per_sec(pwm_dev, PWM_CHANNEL, &cycles_per_sec);

	/* Same "sample rate" concept as the bare-metal version: the PWM
	 * slice wraps (and DMA supplies the next duty value) at 48 kHz,
	 * while the wave itself stays slow enough to see on the LED.
	 */
	uint32_t period_cycles = (uint32_t)(cycles_per_sec / 48000);
	pwm_top = period_cycles - 1;
	pwm_set_cycles(pwm_dev, PWM_CHANNEL, period_cycles, 0, 0);

	cc_half = (volatile uint16_t *)&pwm_hw->slice[PWM_SLICE].cc + 1;

	phase_inc = (uint32_t)((double)WAVE_FREQ_HZ / 48000.0 * 4294967296.0);
	printk("cycles_per_sec=%llu period_cycles=%u pwm_top=%u phase_inc=%u\n",
	       cycles_per_sec, period_cycles, pwm_top, phase_inc);
	fill_buffer(buf[0]);
	fill_buffer(buf[1]);
	playing = 0;

	int ch = dma_request_channel(dma_dev, NULL);
	if (ch < 0) {
		printk("no free dma channel\n");
		return 0;
	}
	dma_channel = (uint32_t)ch;

	struct dma_block_config block = {
		.source_address = (uintptr_t)buf[0],
		.dest_address = (uintptr_t)cc_half,
		.block_size = BUF_LEN * sizeof(uint16_t),
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	};
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.source_data_size = 2,
		.dest_data_size = 2,
		.source_burst_length = 1,
		.dest_burst_length = 1,
		.block_count = 1,
		.head_block = &block,
		.dma_slot = RPI_PICO_DMA_SLOT_PWM_WRAP4,
		.dma_callback = dma_done,
	};

	dma_config(dma_dev, dma_channel, &cfg);
	dma_start(dma_dev, dma_channel);

	printk("DMA+PWM breathing LED running\n");
	while (1) {
		k_msleep(1000);
	}
	return 0;
}
