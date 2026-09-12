/*
 * Step 4: real Amiga MOD playback, same PIO/DMA I2S backend as step 3.
 *
 * mod_player.c/h (ported here unmodified from ~/devel/pico_mplay - it was
 * already pure C with zero pico-sdk/Zephyr dependency) replaces the sweep
 * generator as the sample source. mod_player_produce() doesn't care how
 * many samples are requested or when, so it drops straight into
 * fill_buffer() exactly like it does in the bare-metal version - the only
 * change from step 3 is what fill_buffer() calls to get mono samples
 * before packing them into stereo I2S frames.
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
 * The MOD file (mod_data[]/mod_data_len) is generated at build time from
 * AXEL_F.MOD into the build directory - see CMakeLists.txt.
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

#include "i2s_out_pio.h"
#include "mod_player.h"
#include "mod_data.h"

#define PIN_BCLK 26   /* WS/LRCLK is PIN_BCLK+1 (GP27) - side-set requirement */
#define PIN_DATA 28

#define SAMPLE_RATE_HZ   48000u
#define BITS_PER_CHANNEL 16u

#define BUF_LEN 256   /* stereo frames/buffer */

static const struct device *dma_dev;
static PIO pio;
static size_t sm;
static uint32_t dma_channel;
static volatile uint32_t *txf;

static uint32_t buf[2][BUF_LEN];
static int playing;

static struct PlayerState player;
static int16_t mono_scratch[BUF_LEN];

static void fill_buffer(uint32_t *b)
{
	mod_player_produce(&player, mono_scratch, BUF_LEN);
	for (int i = 0; i < BUF_LEN; i++) {
		uint16_t sample = (uint16_t)mono_scratch[i];
		b[i] = ((uint32_t)sample << 16) | sample;
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

	if (mod_player_init(&player, mod_data, mod_data_len) != 0) {
		printk("mod_player_init failed - bad or unrecognized MOD data\n");
		return 0;
	}

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

	printk("mod player running\n");
	while (1) {
		printk("pos=%d row=%d\n", player.current_position, player.current_row);
		k_msleep(1000);
	}
	return 0;
}
