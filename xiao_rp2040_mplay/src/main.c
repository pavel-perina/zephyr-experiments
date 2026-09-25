/*
 * XIAO RP2040 + Seeeduino expansion board: mod_player.c driving the board's
 * passive piezo buzzer (A3/D3 = GPIO29) via PWM, instead of pico_mplay's
 * I2S DAC output. mod_player.c/.h are copied here unmodified - the only
 * thing that changes going from I2S to PWM is what a "sample" becomes on
 * its way out: instead of packing a 16-bit signed sample into a stereo I2S
 * frame, it's rescaled into an unsigned PWM duty-cycle value.
 *
 * Reuses the same DMA architecture as pico_mplay's step 2 (DMA + PWM) and
 * its own current main.c: Zephyr's pwm.h has no DMA-fed duty-stream concept
 * (one-shot set-period/pulse only), so DMA writes duty values directly into
 * the raw pwm_hw register after Zephyr's PWM driver sets the period once,
 * and the DMA channel is manually re-armed from its own completion
 * callback (this SoC's dma.h has no working "repeat forever"/cyclic mode).
 *
 * Unlike step 2's sine-wave test - which rendered directly inside that
 * callback as a documented simplification ("fine for a small, cheap sine
 * fill; a real (audio-rate) player would defer this") - this is that real
 * audio-rate player, so it uses pico_mplay's proven split instead: the ISR
 * only re-arms DMA and does consumed/filled sequence-counter bookkeeping;
 * mod_player_produce() (the whole 4-channel mixer) runs in main()'s thread,
 * woken by a semaphore. See pico_mplay/src/main.c's comments for why a
 * single flag isn't enough and why rendering can't live in the ISR.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/barrier.h>
#if defined(CONFIG_SOC_SERIES_RP2350)
#include <zephyr/dt-bindings/dma/rpi-pico-dma-rp2350.h>
#else
#include <zephyr/dt-bindings/dma/rpi-pico-dma-rp2040.h>
#endif
#include <hardware/structs/pwm.h>
#include <hardware/structs/dma.h>

#include "mod_player.h"
#include "mod_data.h"
#include "vu_neopixel.h"
#include "oled_display.h"
#include "spectrum.h"
#include "diag.h"

/* A3/D3 = GPIO29 = PWM slice 6, channel B (slice = (gpio>>1)&7, channel =
 * gpio&1). Zephyr's PWM "channel" numbering is slice*2 + (0=A, 1=B).
 */
#define PWM_SLICE   6
#define PWM_CHANNEL (PWM_SLICE * 2 + 1)

#define SAMPLE_RATE_HZ 48000u
#define BUF_LEN        256u   /* samples/buffer */

static const struct device *pwm_dev;
static const struct device *dma_dev;
static uint32_t dma_channel;
static volatile uint16_t *cc_half;   /* channel B = high half of slice[6].cc */
static uint32_t pwm_top;

static uint16_t buf[2][BUF_LEN];

/* Same consumed/filled scheme as pico_mplay/src/main.c - see there for why.
 * The DMA ISR advances `consumed` and re-arms immediately; the main thread
 * advances `filled` only after rendering, so comparing the two catches every
 * underrun including DMA reaching a buffer still mid-render.
 */
static volatile uint32_t consumed;
static volatile uint32_t filled;
static volatile uint32_t underrun_count;
/* Diagnostics for the "audio + spectrum freeze, console keeps printing"
 * hang: dma_done() used to return silently on an error status, never
 * re-arming, which would freeze consumed (and so both audio and the
 * spectrum ring buffer) while main() kept printing stale pos/row.
 */
static volatile uint32_t dma_error_count;
static volatile int dma_last_error;
static volatile uint32_t dma_irq_count;
/* Spectrum thread heartbeat + display_write() health. */
static volatile uint32_t spectrum_loops;
static volatile uint32_t oled_errors;
static volatile int oled_last_error;

K_SEM_DEFINE(fill_needed_sem, 0, 1);

static struct PlayerState player;
static int16_t mono_scratch[BUF_LEN];

/* mod_player.c's MIX_SCALE is shared across every target and deliberately
 * keeps headroom for the I2S DAC/amp chain (see pico_dma/README.md: "keeps
 * 6 dB of signal-to-noise at the DAC/amp instead of giving it away as
 * headroom"). That headroom is exactly wrong here: this is a bare,
 * unamplified piezo driven straight off a GPIO, with no downstream gain
 * stage at all, so the mixer's modest typical amplitude barely swings the
 * PWM duty cycle - likely why it was silent rather than merely quiet.
 * Recovered locally, per the same principle as the earlier volume-control
 * discussion (gain belongs at the output stage, not the shared mixer):
 * heavy clipping here is fine, even expected - a piezo buzzer isn't a
 * hi-fi transducer, and a near-full square-wave swing is what actually
 * moves it audibly. Starting value, likely needs tuning by ear.
 */
#define BUZZER_GAIN 4

/* Renders BUF_LEN mono samples from the MOD mixer, then rescales each one
 * from mod_player's signed 16-bit PCM range into an unsigned PWM duty value
 * in [0, pwm_top] - the buzzer analogue of pico_mplay's "pack into a stereo
 * I2S frame" step.
 */

static void fill_buffer(uint16_t *b)
{
	mod_player_produce(&player, mono_scratch, BUF_LEN);
	/* Temporarily disabled while chasing underruns - not the likely
	 * cause (cheap, ~30us PIO write) but ruling it out. Not committing
	 * this alone.
	 */
	/* vu_neopixel_update(mono_scratch, BUF_LEN); */

	/* Cheap (just decimates into a ring buffer) - stays inline here, same
	 * as vu_neopixel_update(). The FFT itself and the OLED push run in
	 * their own thread (spectrum_thread() below), not every buffer.
	 */
	spectrum_accumulate(mono_scratch, BUF_LEN);

	for (int i = 0; i < BUF_LEN; i++) {
		int32_t sample = (int32_t)mono_scratch[i] * BUZZER_GAIN;

		if (sample > 32767) {
			sample = 32767;
		} else if (sample < -32768) {
			sample = -32768;
		}

		int32_t shifted = sample + 32768;   /* -> [0, 65535] */
		uint32_t duty = ((uint32_t)shifted * (pwm_top + 1)) >> 16;

		if (duty > pwm_top) {
			duty = pwm_top;   /* guard the rounding-up edge case at full scale */
		}
		b[i] = (uint16_t)duty;
	}
}

static void dma_done(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);
	dma_irq_count++;
	if (status < 0) {
		/* Count it but keep going - returning here without re-arming
		 * kills audio permanently.
		 */
		dma_error_count++;
		dma_last_error = status;
	}

	uint32_t next = consumed + 1;

	dma_reload(dma_dev, dma_channel, (uintptr_t)buf[next & 1], (uintptr_t)cc_half,
		   BUF_LEN * sizeof(uint16_t));
	dma_start(dma_dev, dma_channel);

	if ((int32_t)(filled - next) <= 0) {   /* signed diff: wrap-safe */
		underrun_count++;
	}
	consumed = next;

	k_sem_give(&fill_needed_sem);
}

/* OLED bar-graph height cap - passed as both spectrum_get_levels()'s
 * max_value and oled_draw_bars()'s max_height.
 */
#define SPECTRUM_BAR_MAX 64

/* The FFT (spectrum_process()) and the OLED push (oled_draw_bars()) are
 * both too slow for the render thread's ~5.33ms/buffer budget - this
 * thread does them on its own schedule instead, decoupled from audio
 * timing entirely. Lower priority than main(): Zephyr's preemptive
 * scheduler always lets the render thread win whenever dma_done() signals
 * it, no matter how long this thread is mid-FFT or blocked on the OLED's
 * I2C write - see spectrum.c/oled_display.c for why that makes both safe
 * to run unchunked here.
 */
static void spectrum_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		spectrum_process();

		uint8_t bar_heights[SPECTRUM_BANDS];

		spectrum_get_levels(bar_heights, SPECTRUM_BAR_MAX);
		int ret = oled_draw_bars(bar_heights, SPECTRUM_BANDS, SPECTRUM_BAR_MAX);

		if (ret != 0) {
			oled_errors++;
			oled_last_error = ret;
		}
		spectrum_loops++;

		k_msleep(40);
	}
}

/* 4096, not the 2048 this was first sized at: that was picked back when
 * SPECTRUM_BANDS was still 16 (bar_heights was a 16-byte stack array) and
 * never revisited after bumping to 128 bands (128 bytes) a few commits
 * later - on top of the FFT's own frame and however deep the display/I2C
 * driver call chain goes (display_write -> ssd1306_write -> ... ->
 * i2c_dw_transfer). RP2040/Cortex-M0 has no MPU stack guard (same
 * reasoning as the main-thread stack fix earlier in this project), so an
 * overflow here wouldn't panic cleanly - it'd silently corrupt whatever's
 * next to this thread's stack, consistent with a hang with no crash
 * message and an unrelated subsystem (audio) freezing shortly after.
 */
/* Created suspended (K_TICKS_FOREVER) and started from main() only after
 * oled_display_init(): otherwise this thread runs as soon as main() blocks
 * on the init blank's I2C write and pushes a frame concurrently with it,
 * interleaving two SSD1306 command/data sequences.
 */
K_THREAD_DEFINE(spectrum_tid, 4096 + 256, spectrum_thread, NULL, NULL, NULL,
		 K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_TICKS_FOREVER);

/* The XIAO's RGB user LED (led0/1/2 = blue GPIO25, green GPIO16, red
 * GPIO17, all active-low). Left unconfigured, those pins come up as inputs
 * with the RP2040's default pull-down, which sinks enough current through
 * the LEDs to light them - drive them to their inactive (high) level
 * instead.
 */
static void user_leds_off(void)
{
	const struct gpio_dt_spec leds[] = {
		GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
		GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
		GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
	};

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (gpio_is_ready_dt(&leds[i])) {
			gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		}
	}
}

int main(void)
{
	user_leds_off();
	diag_init();

	pwm_dev = DEVICE_DT_GET(DT_NODELABEL(pwm));
	dma_dev = DEVICE_DT_GET(DT_NODELABEL(dma));

	if (!device_is_ready(pwm_dev) || !device_is_ready(dma_dev)) {
		printk("pwm or dma device not ready\n");
		return 0;
	}

	uint64_t cycles_per_sec;

	pwm_get_cycles_per_sec(pwm_dev, PWM_CHANNEL, &cycles_per_sec);

	/* PWM slice wraps (and DMA supplies the next duty value) at exactly
	 * SAMPLE_RATE_HZ - same "sample rate via wrap DREQ" concept as
	 * pico_mplay's I2S BCLK derivation, just for a single PWM channel
	 * instead of a bit-clocked serial protocol.
	 */
	uint32_t period_cycles = (uint32_t)(cycles_per_sec / SAMPLE_RATE_HZ);

	pwm_top = period_cycles - 1;
	pwm_set_cycles(pwm_dev, PWM_CHANNEL, period_cycles, 0, 0);
	cc_half = (volatile uint16_t *)&pwm_hw->slice[PWM_SLICE].cc + 1;

	printk("cycles_per_sec=%llu period_cycles=%u pwm_top=%u\n",
	       cycles_per_sec, period_cycles, pwm_top);

	if (mod_player_init(&player, mod_data, mod_data_len) != 0) {
		printk("mod_player_init failed - bad or unrecognized MOD data\n");
		return 0;
	}

	if (vu_neopixel_init() != 0) {
		printk("NeoPixel not ready - continuing without the VU meter\n");
	}

	if (oled_display_init() != 0) {
		printk("OLED not ready or failed to blank\n");
	}
	k_thread_start(spectrum_tid);

	fill_buffer(buf[0]);
	fill_buffer(buf[1]);
	filled = 2;

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
		.dma_slot = RPI_PICO_DMA_SLOT_PWM_WRAP6,
		.dma_callback = dma_done,
	};

	dma_config(dma_dev, dma_channel, &cfg);
	dma_start(dma_dev, dma_channel);

	printk("mod player running (buzzer/PWM)\n");
	int64_t next_report = k_uptime_get() + 1000;
	uint32_t last_irq_count = 0;

	while (1) {
		/* Woken by dma_done() (or times out - not otherwise fatal) so
		 * the report below still fires ~1/s even if nothing is due
		 * to render this tick.
		 */
		k_sem_take(&fill_needed_sem, K_MSEC(100));
		diag_feed();

		while ((int32_t)(consumed + 2 - filled) > 0) {
			fill_buffer(buf[filled & 1]);
			barrier_dmem_fence_full();
			filled++;
		}

		/* Software-only sanity check, same as pico_mplay: position/
		 * row should visibly advance regardless of whether anything
		 * is audible, plus the underrun count.
		 */
		if (k_uptime_get() >= next_report) {
			printk("pos=%d row=%d underruns=%u dma_irqs=%u dma_errs=%u(%d) "
			       "spec_loops=%u oled_errs=%u(%d)\n",
			       player.current_position, player.current_row,
			       underrun_count, dma_irq_count, dma_error_count,
			       dma_last_error, spectrum_loops, oled_errors,
			       oled_last_error);
			if (dma_irq_count == last_irq_count) {
				/* DMA chain stalled - dump raw state. CTRL bit 24 =
				 * BUSY, bit 0 = EN, bits 29..31 = READ/WRITE/AHB error.
				 * PWM CSR bit 0 = slice enabled.
				 */
				printk("  STALL: ctrl=%08x count=%u read=%08x "
				       "inte0=%08x intr=%08x pwm_csr=%08x pwm_ctr=%u\n",
				       dma_hw->ch[dma_channel].al1_ctrl,
				       dma_hw->ch[dma_channel].transfer_count,
				       dma_hw->ch[dma_channel].read_addr,
				       dma_hw->inte0, dma_hw->intr,
				       pwm_hw->slice[PWM_SLICE].csr,
				       pwm_hw->slice[PWM_SLICE].ctr);
			}
			last_irq_count = dma_irq_count;
			next_report += 1000;
		}
	}
	return 0;
}
