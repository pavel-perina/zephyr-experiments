/*
 * XIAO + Seeed expansion board MOD player: mod_player.c rendered into the
 * board's passive piezo buzzer (A3/D3), plus an OLED spectrum/scope and the
 * D1 button. Originally pico_mplay's I2S DAC output adapted to PWM - the
 * only thing that changes going from I2S to PWM is what a "sample" becomes
 * on its way out: an unsigned PWM duty-cycle value instead of a stereo I2S
 * frame.
 *
 * The output itself is behind audio.h (audio.c + a per-SoC backend,
 * audio_rp2xxx.c / audio_nrf.c): this file renders into its buffers from
 * the main thread, woken whenever one has been played - the backend's
 * interrupt only moves buffers to the hardware, never renders (see
 * pico_mplay/src/main.c for why rendering can't live in the ISR).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>

#include "audio.h"
#include "mod_player.h"
#include "mod_table.h"
#include "vu_neopixel.h"
#include "oled_display.h"
#include "spectrum.h"
#include "diag.h"

/* Spectrum thread heartbeat + display_write() health. */
static volatile uint32_t spectrum_loops;
/* Frame work time (FFT + draw + OLED push, excluding the period sleep),
 * summed over the frames since the last status report - the report prints
 * the average as frame_us: the headroom left in SPECTRUM_FRAME_MS.
 */
static volatile uint32_t frame_work_us_sum;
static volatile uint32_t frame_work_count;
static volatile uint32_t oled_errors;
static volatile int oled_last_error;

static struct PlayerState player;
static size_t song_idx;

/* D1 button on the expansion board: the shield's gpio-keys node, which
 * already debounces (debounce-interval-ms, 30ms default - raised to 50ms in
 * boards/xiao_rp2040.overlay) and reports press/release as INPUT_KEY_0
 * events from the input thread.
 *
 *  - short press (released before LONG_PRESS_MS): next song. Acted on at
 *    release, since only then is it known to be short. This only raises a
 *    flag: switching songs reinitialises `player`, which must happen in
 *    main()'s render thread between fill_buffer() calls, never concurrently
 *    with one.
 *  - long press: cycles the display (fine -> wide -> hires -> scope) as soon as
 *    LONG_PRESS_MS elapses, while still held - no need to guess when to
 *    let go. spectrum_set_mode() is safe from any thread.
 *
 * button_state decides which of the two a press becomes, exactly once:
 * the release handler and the long-press timer both try to move it out of
 * BTN_PRESSED with a compare-and-swap, and only the winner acts.
 */
#define LONG_PRESS_MS 600

enum { BTN_IDLE, BTN_PRESSED, BTN_LONG };

static atomic_t next_song_requested;
static atomic_t button_state = ATOMIC_INIT(BTN_IDLE);

static void long_press_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (atomic_cas(&button_state, BTN_PRESSED, BTN_LONG)) {
		enum spectrum_mode mode = (spectrum_get_mode() + 1) % SPECTRUM_MODE_COUNT;

		spectrum_set_mode(mode);
		printk("display: %s\n", spectrum_mode_name(mode));
	}
}
static K_WORK_DELAYABLE_DEFINE(long_press_work, long_press_handler);

static void button_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY || evt->code != INPUT_KEY_0) {
		return;
	}
	if (evt->value) {
		atomic_set(&button_state, BTN_PRESSED);
		k_work_schedule(&long_press_work, K_MSEC(LONG_PRESS_MS));
	} else if (atomic_cas(&button_state, BTN_PRESSED, BTN_IDLE)) {
		/* released before the long-press timer fired: short press */
		k_work_cancel_delayable(&long_press_work);
		atomic_set(&next_song_requested, 1);
	} else {
		atomic_set(&button_state, BTN_IDLE);   /* end of a long press */
	}
}
INPUT_CALLBACK_DEFINE(NULL, button_cb, NULL);

/* Loads mod_table[idx], or the next one that parses if it doesn't (a failed
 * mod_player_init() leaves `player` zeroed - header NULL - which
 * mod_player_produce() must never see). Returns false if none do.
 */
static bool load_song(size_t idx)
{
	for (size_t tries = 0; tries < ARRAY_SIZE(mod_table); tries++) {
		const struct mod_entry *m = &mod_table[idx];

		if (mod_player_init(&player, m->data, m->len) == 0) {
			song_idx = idx;
			printk("song %u/%u: %s (%u bytes)\n", (unsigned)idx + 1,
			       (unsigned)ARRAY_SIZE(mod_table), m->name, (unsigned)m->len);
			return true;
		}
		printk("song %s: mod_player_init failed, skipping\n", m->name);
		idx = (idx + 1) % ARRAY_SIZE(mod_table);
	}
	return false;
}
static int16_t mono_scratch[AUDIO_BUF_LEN];

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

/* Renders AUDIO_BUF_LEN mono samples from the MOD mixer, feeds the
 * spectrum, applies BUZZER_GAIN and queues them for output.
 */
static void fill_buffer(void)
{
	mod_player_produce(&player, mono_scratch, AUDIO_BUF_LEN);
	/* Temporarily disabled while chasing underruns - not the likely
	 * cause (cheap, ~30us PIO write) but ruling it out. Not committing
	 * this alone.
	 */
	/* vu_neopixel_update(mono_scratch, AUDIO_BUF_LEN); */

	/* Cheap (just decimates into a ring buffer) - stays inline here, same
	 * as vu_neopixel_update(). The FFT itself and the OLED push run in
	 * their own thread (spectrum_thread() below), not every buffer. Fed
	 * the mixer's output before BUZZER_GAIN, as before.
	 */
	spectrum_accumulate(mono_scratch, AUDIO_BUF_LEN);

	for (int i = 0; i < AUDIO_BUF_LEN; i++) {
		int32_t sample = (int32_t)mono_scratch[i] * BUZZER_GAIN;

		if (sample > 32767) {
			sample = 32767;
		} else if (sample < -32768) {
			sample = -32768;
		}
		mono_scratch[i] = (int16_t)sample;
	}
	audio_submit(mono_scratch);
}

/* OLED bar-graph height cap - passed as both spectrum_get_levels()'s
 * max_value and oled_draw_bars()'s max_height.
 */
#define SPECTRUM_BAR_MAX 64

/* Display frame period - spectrum.c's envelope decay is tuned for it. */
#define SPECTRUM_FRAME_MS 40

/* Oscilloscope trace size - the whole OLED. */
#define SCOPE_WIDTH  128
#define SCOPE_HEIGHT 64

/* The FFT (spectrum_process()) and the OLED push (oled_draw_bars()) are
 * both too slow for the render thread's ~5.33ms/buffer budget - this
 * thread does them on its own schedule instead, decoupled from audio
 * timing entirely. Lower priority than main(): Zephyr's preemptive
 * scheduler always lets the render thread win whenever the audio backend signals
 * it, no matter how long this thread is mid-FFT or blocked on the OLED's
 * I2C write - see spectrum.c/oled_display.c for why that makes both safe
 * to run unchunked here.
 */
static void spectrum_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int64_t next_frame = k_uptime_get();

	while (1) {
		int ret;
		uint32_t work_start = k_cycle_get_32();

		if (spectrum_process() == SPECTRUM_MODE_SCOPE) {
			uint8_t ys[SCOPE_WIDTH];

			spectrum_get_scope(ys, SCOPE_WIDTH, SCOPE_HEIGHT);
			ret = oled_draw_trace(ys, SCOPE_WIDTH);
		} else {
			uint8_t bar_heights[SPECTRUM_MAX_BANDS];
			uint8_t bar_peaks[SPECTRUM_MAX_BANDS];

			spectrum_get_levels(bar_heights, bar_peaks, SPECTRUM_BAR_MAX);
			ret = oled_draw_bars(bar_heights, bar_peaks, spectrum_band_count(),
					     SPECTRUM_BAR_MAX);
		}

		if (ret != 0) {
			oled_errors++;
			oled_last_error = ret;
		}
		spectrum_loops++;
		frame_work_us_sum += k_cyc_to_us_floor32(k_cycle_get_32() - work_start);
		frame_work_count++;

		/* Fixed 40ms frame period (25 fps) - sleep only for what's left
		 * of it. This used to be a flat k_msleep(40) *after* the work,
		 * so the real period was 40ms + FFT + the ~23ms I2C push: ~58ms
		 * (~17 fps) on the RP2040, ~80ms (~12 fps) on the XIAO BLE - and
		 * spectrum.c's DECAY_ALPHA_Q14, computed for exactly 40ms steps,
		 * made the bars fall correspondingly slower than designed. If a
		 * frame overruns the period, carry on from now rather than
		 * rushing to catch up.
		 */
		next_frame += SPECTRUM_FRAME_MS;
		int64_t left = next_frame - k_uptime_get();

		if (left > 0) {
			k_msleep((int32_t)left);
		} else {
			next_frame = k_uptime_get();
		}
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

/* The XIAO's user LEDs (RP2040: an RGB trio, led0/1/2 = blue GPIO25,
 * green GPIO16, red GPIO17, all active-low; RP2350: a single led0). Left
 * unconfigured, those pins come up as inputs with the RP2040's default
 * pull-down, which sinks enough current through the LEDs to light them -
 * drive them to their inactive level instead. Only the aliases the board
 * actually defines are used.
 */
#define USER_LED_SPEC(alias) GPIO_DT_SPEC_GET(DT_ALIAS(alias), gpios),

static void user_leds_off(void)
{
	const struct gpio_dt_spec leds[] = {
		IF_ENABLED(DT_NODE_EXISTS(DT_ALIAS(led0)), (USER_LED_SPEC(led0)))
		IF_ENABLED(DT_NODE_EXISTS(DT_ALIAS(led1)), (USER_LED_SPEC(led1)))
		IF_ENABLED(DT_NODE_EXISTS(DT_ALIAS(led2)), (USER_LED_SPEC(led2)))
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

	if (audio_init() != 0) {
		printk("audio output init failed\n");
		return 0;
	}

	if (!load_song(0)) {
		printk("no playable MOD in mod_table\n");
		return 0;
	}

	if (vu_neopixel_init() != 0) {
		printk("NeoPixel not ready - continuing without the VU meter\n");
	}

	if (oled_display_init() != 0) {
		printk("OLED not ready or failed to blank\n");
	}
	k_thread_start(spectrum_tid);

	while (audio_wants_buffer()) {
		fill_buffer();   /* prefill both buffers */
	}
	audio_start();

	printk("mod player running (buzzer, %u Hz)\n", audio_sample_rate());
	int64_t next_report = k_uptime_get() + 1000;

	while (1) {
		/* Woken whenever a buffer has been played (or times out - not
		 * otherwise fatal) so the report below still fires ~1/s even
		 * if nothing is due to render this tick.
		 */
		audio_wait(K_MSEC(100));
		diag_feed();

		/* Song ended (the player looped it back to the start):
		 * advance like a button press. A few ms of the restart may
		 * already be in the queued buffers - inaudible in practice.
		 */
		if (player.restarts > 0) {
			atomic_set(&next_song_requested, 1);
		}

		if (atomic_clear(&next_song_requested)) {
			/* The two buffers already queued still play out the
			 * old song (~10ms) - not worth flushing.
			 */
			if (!load_song((song_idx + 1) % ARRAY_SIZE(mod_table))) {
				printk("no playable MOD in mod_table\n");
				return 0;
			}
		}

		audio_poll();

		while (audio_wants_buffer()) {
			fill_buffer();
		}

		/* Software-only sanity check, same as pico_mplay: position/
		 * row should visibly advance regardless of whether anything
		 * is audible, plus the underrun count.
		 */
		if (k_uptime_get() >= next_report) {
			struct audio_stats st;

			audio_get_stats(&st);
			/* Racy read of the two sums (other thread) - fine for a
			 * once-a-second diagnostic average.
			 */
			uint32_t n = frame_work_count;
			uint32_t frame_us = n ? frame_work_us_sum / n : 0;

			frame_work_us_sum = 0;
			frame_work_count = 0;
			printk("song=%u pos=%d row=%d underruns=%u irqs=%u errs=%u(%d) "
			       "restarts=%u spec_loops=%u frame_us=%u oled_errs=%u(%d)\n",
			       (unsigned)song_idx + 1,
			       player.current_position, player.current_row,
			       st.underruns, st.irqs, st.errors, st.last_error,
			       st.restarts, spectrum_loops, frame_us, oled_errors,
			       oled_last_error);
			next_report += 1000;
		}
	}
	return 0;
}
