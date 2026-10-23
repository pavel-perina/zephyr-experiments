/*
 * Vector table and reset handler. On Cortex-M the hardware loads the stack
 * pointer from vectors[0] before running the reset handler, so all of this
 * can be C.
 */
#include <stdint.h>
#include <nrf.h>

/* Defined in link.ld */
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

int main(void);
void SysTick_Handler(void);

void Reset_Handler(void);

static void Default_Handler(void)
{
	for (;;) {
	}
}

/* 16 core exceptions + the nRF52840's 48 peripheral interrupts. None of
 * the peripheral ones are used, but each entry must hold a valid address:
 * a shorter table would send a stray interrupt into the code after it.
 */
__attribute__((section(".vectors"), used))
static void (*const vectors[64])(void) = {
	(void (*)(void))&_estack,
	Reset_Handler,
	Default_Handler,	/* NMI */
	Default_Handler,	/* HardFault */
	Default_Handler,	/* MemManage */
	Default_Handler,	/* BusFault */
	Default_Handler,	/* UsageFault */
	0, 0, 0, 0,		/* reserved */
	Default_Handler,	/* SVCall */
	Default_Handler,	/* DebugMonitor */
	0,			/* reserved */
	Default_Handler,	/* PendSV */
	SysTick_Handler,
	[16 ... 63] = Default_Handler,
};

void Reset_Handler(void)
{
	/* Initialized globals: copy their values from flash to RAM */
	uint32_t *src = &_sidata;
	for (uint32_t *dst = &_sdata; dst < &_edata;) {
		*dst++ = *src++;
	}
	/* Uninitialized globals: zero */
	for (uint32_t *dst = &_sbss; dst < &_ebss;) {
		*dst++ = 0;
	}

	/* We're started through the MBR and SoftDevice at 0x0, so VTOR still
	 * points at their table; point it at ours for SysTick.
	 */
	SCB->VTOR = (uint32_t)vectors;

	/* Nordic's chip setup (system_nrf52.c): silicon errata workarounds,
	 * sets SystemCoreClock.
	 */
	SystemInit();

	main();
	for (;;) {
	}
}
