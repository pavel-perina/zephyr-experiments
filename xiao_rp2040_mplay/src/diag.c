/*
 * Crash diagnostics that survive the USB CDC console.
 *
 * A fatal error (fault, stack sentinel/MPU guard hit, k_panic) locks
 * interrupts and halts - the CDC-ACM TX work item that would send the fault
 * dump to the host never runs again, so nothing is ever printed. Instead,
 * the fatal handler stashes the essentials in RAM that the next boot doesn't
 * clear (__noinit) and reboots; diag_init() prints them once USB is back.
 *
 * A hang that isn't a fatal error (deadlock, IRQ storm) is caught by the
 * hardware watchdog, fed from main()'s loop - the reset cause printed at boot
 * then says "watchdog" rather than "pin/power-on".
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/reboot.h>

#include "diag.h"

#define CRASH_MAGIC 0xC0FFEE42u

struct crash_record {
	uint32_t magic;
	uint32_t reason;
	uint32_t pc;
	uint32_t lr;
	uint32_t uptime_ms;
	char thread[16];
};

static __noinit struct crash_record crash;

static const struct device *wdt_dev = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	crash.reason = reason;
	crash.pc = esf ? esf->basic.pc : 0;
	crash.lr = esf ? esf->basic.lr : 0;
	crash.uptime_ms = k_uptime_get_32();

	const char *name = k_thread_name_get(k_current_get());

	strncpy(crash.thread, name ? name : "?", sizeof(crash.thread) - 1);
	crash.thread[sizeof(crash.thread) - 1] = '\0';
	crash.magic = CRASH_MAGIC;

	sys_reboot(SYS_REBOOT_COLD);
}

void diag_init(void)
{
	/* Give the host a moment to open the CDC port after enumeration,
	 * otherwise this one-shot boot report is usually lost.
	 */
	k_msleep(2000);

	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) == 0) {
		printk("reset cause: 0x%08x%s%s%s%s\n", cause,
		       (cause & RESET_POR) ? " POR" : "",
		       (cause & RESET_PIN) ? " PIN" : "",
		       (cause & RESET_WATCHDOG) ? " WATCHDOG" : "",
		       (cause & RESET_DEBUG) ? " DEBUG" : "");
		hwinfo_clear_reset_cause();
	}

	if (crash.magic == CRASH_MAGIC) {
		printk("*** previous run crashed: reason=%u pc=0x%08x lr=0x%08x "
		       "thread=%s uptime=%ums\n",
		       crash.reason, crash.pc, crash.lr, crash.thread,
		       crash.uptime_ms);
		printk("    decode: arm-zephyr-eabi-addr2line -e build/zephyr/zephyr.elf "
		       "0x%08x 0x%08x\n", crash.pc, crash.lr);
	} else {
		printk("no crash record from previous run\n");
	}
	crash.magic = 0;

	if (!device_is_ready(wdt_dev)) {
		printk("watchdog not ready\n");
		return;
	}

	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0, .max = 2000 },
		.flags = WDT_FLAG_RESET_SOC,
	};

	wdt_channel = wdt_install_timeout(wdt_dev, &cfg);
	if (wdt_channel < 0 || wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
		printk("watchdog setup failed\n");
		wdt_channel = -1;
	}
}

void diag_feed(void)
{
	if (wdt_channel >= 0) {
		wdt_feed(wdt_dev, wdt_channel);
	}
}
