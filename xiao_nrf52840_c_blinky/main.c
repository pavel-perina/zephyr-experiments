/*
 * Blink the XIAO nRF52840's blue user LED (P0.06, active low), timed by
 * the SysTick interrupt (1 ms).
 */
#include <stdint.h>
#include <nrf.h>

#define LED_PIN 6

static volatile uint32_t ticks;

void SysTick_Handler(void)
{
	ticks++;
}

static void delay_ms(uint32_t ms)
{
	uint32_t start = ticks;

	/* Busy-wait: SysTick runs on the CPU clock, which the nRF52 stops
	 * during WFI sleep, so sleeping here would never wake up. Sleeping
	 * delays need a peripheral timer (RTC), as Zephyr uses.
	 */
	while (ticks - start < ms) {
	}
}

int main(void)
{
	NRF_P0->DIRSET = 1 << LED_PIN;	/* output; OUT is 0, so LED on */
	SysTick_Config(SystemCoreClock / 1000);

	for (;;) {
		NRF_P0->OUT ^= 1 << LED_PIN;
		delay_ms(500);
	}
}
