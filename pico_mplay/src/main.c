/*
 * Step 3: PIO I2S transmitter + DMA, real audio to a MAX98357A speaker.
 *
 * Reuses ~/devel/pico_mplay's i2s_out.pio verbatim (see i2s_out_pio.h) via
 * Zephyr's pio_rpi_pico driver, which handles state-machine allocation and
 * program loading but - like Zephyr's PWM API in step 2 - has no concept
 * of a DMA-fed streaming consumer for a custom PIO program. So the pattern
 * is the same as step 2: Zephyr's driver does the one-time setup (here,
 * allocating the SM and loading the program), then DMA is configured and
 * manually re-armed from its own completion callback via zephyr/drivers/dma.h,
 * writing 32-bit stereo frames directly into the PIO's TX FIFO register.
 *
 * pio_gpio_init()/pio_sm_set_consecutive_pindirs() (raw pico-sdk calls, same
 * as the bare-metal version) handle routing GP26/27/28 to the PIO block
 * directly - no Zephyr pinctrl devicetree group needed for this peripheral.
 *
 * Test signal: the same repeating logarithmic sweep (20Hz-20kHz over 5s)
 * used to first bring up the bare-metal I2S transmitter - a more
 * informative smoke test than a fixed tone, since wrong wiring/timing
 * tends to produce silence, noise, or an audibly broken sweep rather than
 * a clean one.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#if defined(CONFIG_SOC_SERIES_RP2350)
#include <zephyr/dt-bindings/dma/rpi-pico-dma-rp2350.h>
#else
#include <zephyr/dt-bindings/dma/rpi-pico-dma-rp2040.h>
#endif
#include <hardware/pio.h>
/*
 * pico-sdk's hardware/pio.h #defines pio0/pio1 as its own PIO-instance
 * singletons (`#define pio0 pio0_hw`), which collides with Zephyr's
 * DT_NODELABEL(pio0) token-pasting macro below - both want the bare
 * identifier. We never use the pico-sdk singletons (PIO comes from
 * pio_rpi_pico_get_pio() instead), so just get them out of the way.
 */
#undef pio0
#undef pio1
#include <hardware/clocks.h>
#include <math.h>

#include "i2s_out_pio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PIN_BCLK 26   /* WS/LRCLK is PIN_BCLK+1 (GP27) - side-set requirement */
#define PIN_DATA 28

#define SAMPLE_RATE_HZ   48000u
#define BITS_PER_CHANNEL 16u

#define SWEEP_FREQ_START 20.0f
#define SWEEP_FREQ_END   20000.0f
#define SWEEP_DURATION_S 5.0f
#define AMPLITUDE        8000

#define BUF_LEN          256   /* stereo frames/buffer */
#define SINE_TABLE_BITS  10
#define SINE_TABLE_LEN   (1 << SINE_TABLE_BITS)

static const struct device *dma_dev;
static PIO pio;
static size_t sm;
static uint32_t dma_channel;
static volatile uint32_t *txf;

static uint32_t buf[2][BUF_LEN];
static int playing;

static uint32_t phase;
static float phase_inc_scale;
static float sweep_freq_hz;
static float sweep_growth_per_sample;
static uint32_t sweep_sample_count;
static uint32_t sweep_total_samples;
static int16_t sine_table[SINE_TABLE_LEN];

static void fill_buffer(uint32_t *b)
{
	for (int i = 0; i < BUF_LEN; i++) {
		int16_t sample = sine_table[phase >> (32 - SINE_TABLE_BITS)];
		b[i] = ((uint32_t)(uint16_t)sample << 16) | (uint16_t)sample;

		phase += (uint32_t)(sweep_freq_hz * phase_inc_scale);
		sweep_freq_hz *= sweep_growth_per_sample;

		if (++sweep_sample_count >= sweep_total_samples) {
			sweep_freq_hz = SWEEP_FREQ_START;
			sweep_sample_count = 0;
		}
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

	int next = 1 - playing;
	dma_reload(dma_dev, dma_channel, (uintptr_t)buf[next], (uintptr_t)txf,
		   BUF_LEN * sizeof(uint32_t));
	dma_start(dma_dev, dma_channel);
	playing = next;

	fill_buffer(buf[1 - next]);
}

int main(void)
{
	const struct device *pio_dev = DEVICE_DT_GET(DT_NODELABEL(pio0));

	dma_dev = DEVICE_DT_GET(DT_NODELABEL(dma));

	if (!device_is_ready(pio_dev) || !device_is_ready(dma_dev)) {
		printk("pio or dma device not ready\n");
		return 0;
	}

	pio = pio_rpi_pico_get_pio(pio_dev);
	if (pio_rpi_pico_allocate_sm(pio_dev, &sm) != 0) {
		printk("no free PIO state machine\n");
		return 0;
	}

	uint offset = pio_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(i2s_out));
	float clkdiv = (float)clock_get_hz(clk_sys) / (float)(SAMPLE_RATE_HZ * 4u * BITS_PER_CHANNEL);

	pio_gpio_init(pio, PIN_BCLK);
	pio_gpio_init(pio, PIN_BCLK + 1);
	pio_gpio_init(pio, PIN_DATA);
	pio_sm_set_consecutive_pindirs(pio, sm, PIN_BCLK, 2, true);
	pio_sm_set_consecutive_pindirs(pio, sm, PIN_DATA, 1, true);

	pio_sm_config c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, offset + RPI_PICO_PIO_GET_WRAP_TARGET(i2s_out),
			    offset + RPI_PICO_PIO_GET_WRAP(i2s_out));
	sm_config_set_sideset(&c, 2, false, false);
	sm_config_set_sideset_pins(&c, PIN_BCLK);
	sm_config_set_out_pins(&c, PIN_DATA, 1);
	sm_config_set_out_shift(&c, false, true, 32);
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv(&c, clkdiv);
	pio_sm_init(pio, sm, offset, &c);

	for (int i = 0; i < SINE_TABLE_LEN; i++) {
		float angle = 2.0f * (float)M_PI * (float)i / (float)SINE_TABLE_LEN;
		sine_table[i] = (int16_t)(sinf(angle) * (float)AMPLITUDE);
	}

	phase_inc_scale = 4294967296.0f / (float)SAMPLE_RATE_HZ;
	sweep_freq_hz = SWEEP_FREQ_START;
	sweep_total_samples = (uint32_t)(SWEEP_DURATION_S * (float)SAMPLE_RATE_HZ);
	sweep_growth_per_sample =
		powf(SWEEP_FREQ_END / SWEEP_FREQ_START, 1.0f / (float)sweep_total_samples);

	fill_buffer(buf[0]);
	fill_buffer(buf[1]);
	playing = 0;
	txf = &pio->txf[sm];

	int ch = dma_request_channel(dma_dev, NULL);
	if (ch < 0) {
		printk("no free dma channel\n");
		return 0;
	}
	dma_channel = (uint32_t)ch;

	struct dma_block_config block = {
		.source_address = (uintptr_t)buf[0],
		.dest_address = (uintptr_t)txf,
		.block_size = BUF_LEN * sizeof(uint32_t),
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	};
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.source_data_size = 4,
		.dest_data_size = 4,
		.source_burst_length = 1,
		.dest_burst_length = 1,
		.block_count = 1,
		.head_block = &block,
		.dma_slot = RPI_PICO_DMA_DREQ_TO_SLOT(pio_get_dreq(pio, sm, true)),
		.dma_callback = dma_done,
	};

	dma_config(dma_dev, dma_channel, &cfg);

	pio_sm_set_enabled(pio, sm, true);
	dma_start(dma_dev, dma_channel);

	printk("I2S sweep running\n");
	while (1) {
		k_msleep(1000);
	}
	return 0;
}
