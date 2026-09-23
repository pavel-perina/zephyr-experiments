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
 * dma_done() (the DMA completion ISR) only re-arms DMA and does bookkeeping
 * (consumed/filled sequence counters, an underrun count, and the PIO's own
 * TXSTALL flag) - it does not render. Rendering (fill_buffer(), i.e. the
 * whole 4-channel mixer) runs in main()'s thread, woken by a semaphore the
 * ISR gives, so a slow render can't block other same/lower-priority
 * interrupts (e.g. USB CDC) for the whole mixer's runtime. See the
 * consumed/filled comment near their declarations for why a single flag
 * isn't enough, and ~/devel/pico_dma/pico_mplay.c for the bare-metal
 * version of the same scheme this was ported from.
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
#include <zephyr/sys/barrier.h>
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

/* Buffers are numbered by sequence: buffer k lives in buf[k & 1]. The DMA
 * ISR (dma_done()) advances `consumed` (buffers DMA has finished) and
 * re-arms the next one immediately; the main thread advances `filled`
 * (buffers completely rendered) only *after* rendering, so comparing the
 * two counters catches every underrun - including DMA reaching a buffer
 * the main thread is still mid-render on. Mirrors the bare-metal version's
 * consumed/filled scheme in ~/devel/pico_dma/pico_mplay.c, and for the same
 * reason: a single "fill_needed" flag cleared before rendering starts can
 * miss/lose events across the ISR/thread boundary.
 *
 * Unlike bare-metal, rendering used to happen directly inside dma_done()
 * itself (the DMA completion ISR) - the entire 4-channel mixer ran with
 * this interrupt's priority blocked for however long mod_player_produce()
 * took, every 5.33ms. That's moved to the main thread below so a slow
 * render can't stall other interrupts (USB CDC console included).
 */
static volatile uint32_t consumed;   /* ISR-owned */
static volatile uint32_t filled;     /* main-thread-owned */
static volatile uint32_t underrun_count;

/* Hardware's own "FIFO ran dry" flag (PIO's sticky per-SM TXSTALL bit),
 * independent of the consumed/filled bookkeeping above - catches cases the
 * software counters can't, such as the main thread being descheduled
 * entirely rather than merely running behind.
 */
static volatile uint32_t txstall_count;

K_SEM_DEFINE(fill_needed_sem, 0, 1);

static struct PlayerState player;
static int16_t mono_scratch[BUF_LEN];

/* Renders BUF_LEN mono samples from the MOD mixer, then packs each one into
 * a stereo I2S frame (same value in both channels) matching what
 * i2s_out.pio expects from the FIFO: [left:16][right:16] in one 32-bit word.
 */
static void fill_buffer(uint32_t *b)
{
	mod_player_produce(&player, mono_scratch, BUF_LEN);
	for (int i = 0; i < BUF_LEN; i++) {
		uint16_t sample = (uint16_t)mono_scratch[i];
		b[i] = ((uint32_t)sample << 16) | sample;
	}
}

/* Fires once DMA finishes streaming one of the two buffers. Immediately
 * hands the next buffer (already rendered last time round) back to DMA to
 * keep the gap minimal, bumps the sequence counters, and wakes the main
 * thread to render the buffer that just finished playing - it does *not*
 * render here itself (see the consumed/filled comment above for why).
 * Same driver calls as before (dma_reload()/dma_start()), just no longer
 * doing the mixer's work on the way out.
 */
static void dma_done(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);
	if (status < 0) {
		return;
	}

	uint32_t next = consumed + 1;   /* sequence number of the buffer to play now */

	dma_reload(dma_dev, dma_channel, (uintptr_t)buf[next & 1], (uintptr_t)txf,
		   BUF_LEN * sizeof(uint32_t));
	dma_start(dma_dev, dma_channel);

	/* `next` must be completely rendered by now (filled > next). If the
	 * main thread hasn't finished it - or hasn't started - DMA is about
	 * to play stale data.
	 */
	if ((int32_t)(filled - next) <= 0) {   /* signed diff: wrap-safe */
		underrun_count++;
	}
	consumed = next;

	uint32_t stall_bit = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
	if (pio->fdebug & stall_bit) {
		txstall_count++;
		pio->fdebug = stall_bit;   /* write-1-to-clear */
	}

	k_sem_give(&fill_needed_sem);
}

int main(void)
{
	/* &pio0 and &dma are enabled in the per-board overlays under boards/;
	 * pio_dev only exists to allocate a state machine and get the raw
	 * PIO handle - everything else is driven through direct pico-sdk
	 * calls below.
	 */
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

	/* Load the program bytes from i2s_out_pio.h into PIO instruction
	 * memory and get back where it landed (offset).
	 */
	uint offset = pio_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(i2s_out));

	/* i2s_out.pio spends 2 PIO cycles per bit and 2*BITS_PER_CHANNEL bits
	 * per stereo frame, so the SM must run at SAMPLE_RATE_HZ * 4 *
	 * BITS_PER_CHANNEL to make one frame take exactly one sample period -
	 * same formula as the bare-metal version.
	 */
	float clkdiv = (float)clock_get_hz(clk_sys) / (float)(SAMPLE_RATE_HZ * 4u * BITS_PER_CHANNEL);

	/* Route GP26/27 (BCLK/WS) and GP28 (DATA) to this PIO block and set
	 * them as outputs - raw pico-sdk calls, no Zephyr pinctrl involved.
	 */
	pio_gpio_init(pio, PIN_BCLK);
	pio_gpio_init(pio, PIN_BCLK + 1);
	pio_gpio_init(pio, PIN_DATA);
	pio_sm_set_consecutive_pindirs(pio, sm, PIN_BCLK, 2, true);
	pio_sm_set_consecutive_pindirs(pio, sm, PIN_DATA, 1, true);

	/* Same SM configuration as i2s_out_program_init() in the bare-metal
	 * project's generated header - reconstructed by hand here since
	 * Zephyr's generic RPI_PICO_PIO_DEFINE_PROGRAM macro only captures
	 * wrap_target/wrap, not a program-specific default-config helper.
	 */
	pio_sm_config c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, offset + RPI_PICO_PIO_GET_WRAP_TARGET(i2s_out),
			    offset + RPI_PICO_PIO_GET_WRAP(i2s_out));
	sm_config_set_sideset(&c, 2, false, false);          /* BCLK+WS, 2 bits */
	sm_config_set_sideset_pins(&c, PIN_BCLK);
	sm_config_set_out_pins(&c, PIN_DATA, 1);
	sm_config_set_out_shift(&c, false, true, 32);        /* MSB-first, autopull every 32 bits */
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);       /* 8-deep TX FIFO, no RX needed */
	sm_config_set_clkdiv(&c, clkdiv);
	pio_sm_init(pio, sm, offset, &c);

	if (mod_player_init(&player, mod_data, mod_data_len) != 0) {
		printk("mod_player_init failed - bad or unrecognized MOD data\n");
		return 0;
	}

	/* Pre-fill both buffers before DMA/PIO start moving, then remember
	 * the PIO's TX FIFO address as the fixed DMA destination.
	 */
	fill_buffer(buf[0]);
	fill_buffer(buf[1]);
	filled = 2;
	txf = &pio->txf[sm];

	int ch = dma_request_channel(dma_dev, NULL);
	if (ch < 0) {
		printk("no free dma channel\n");
		return 0;
	}
	dma_channel = (uint32_t)ch;

	/* One 32-bit stereo frame per transfer, source address walks buf[0],
	 * destination is the PIO FIFO register and never moves.
	 */
	struct dma_block_config block = {
		.source_address = (uintptr_t)buf[0],
		.dest_address = (uintptr_t)txf,
		.block_size = BUF_LEN * sizeof(uint32_t),
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	};
	/* dma_slot paces the channel from the PIO SM's own TX DREQ (fires
	 * whenever the FIFO has room), same DREQ pio_get_dreq() would give
	 * the bare-metal version - just wrapped for Zephyr's dma_slot field.
	 */
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

	/* Enable the SM before the first DMA transfer, same order as the
	 * bare-metal version, so it's already waiting on the FIFO rather
	 * than racing the first transfer.
	 */
	pio_sm_set_enabled(pio, sm, true);
	dma_start(dma_dev, dma_channel);
	/* The SM was enabled before the first transfer started, so it stalls
	 * once waiting for it - clear that expected startup stall before
	 * txstall_count starts meaning "FIFO ran dry during playback".
	 */
	pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);

	printk("mod player running\n");
	int64_t next_report = k_uptime_get() + 1000;
	while (1) {
		/* Woken by dma_done() (or times out - not otherwise fatal) so
		 * the report below still fires ~1/s even if nothing is due
		 * to render this tick.
		 */
		k_sem_take(&fill_needed_sem, K_MSEC(100));

		/* Buffer `filled` may be rendered once buffer `filled - 2`
		 * (same slot) has been consumed, i.e. filled < consumed + 2.
		 */
		while ((int32_t)(consumed + 2 - filled) > 0) {
			fill_buffer(buf[filled & 1]);
			barrier_dmem_fence_full();   /* buffer stores land before the count says "done" */
			filled++;
		}

		/* Software-only sanity check, same as bare-metal: position/
		 * row should visibly advance through the song regardless of
		 * whether anything is audible, plus both underrun indicators
		 * (software: render ran late; hardware: FIFO actually ran
		 * dry) so a symptom like an occasional pop can be attributed
		 * to this DMA/render path or ruled out in favour of something
		 * else (e.g. a power-supply issue affecting the amp/DAC).
		 */
		if (k_uptime_get() >= next_report) {
			printk("pos=%d row=%d underruns=%u txstall=%u\n",
			       player.current_position, player.current_row,
			       underrun_count, txstall_count);
			next_report += 1000;
		}
	}
	return 0;
}
