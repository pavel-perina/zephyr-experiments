// Blink the XIAO nRF52840's blue user LED (P0.06, active low) - no SDK,
// no Zephyr, just registers. The CPU runs at 64 MHz from the internal
// oscillator out of reset; the delay is approximate.
//
// Linked at 0x27000: 0x0-0x27000 holds the MBR and SoftDevice, which the
// Adafruit UF2 bootloader (at 0xf4000) expects there; it starts the
// application through them, reading our vector table at 0x27000.

    .cpu cortex-m4
    .thumb
    .syntax unified

    .equ P0_BASE,   0x50000000
    .equ OUT,       0x504
    .equ DIRSET,    0x518

    .equ LED_MASK,  1 << 6
    .equ DELAY,     10000000  // ~3 cycles per loop: ~0.5 s at 64 MHz

    // Vector table: initial SP, then reset handler (bit 0 set = Thumb).
    .section .vectors, "a"
    .word 0x20040000                  // top of the 256 KB SRAM
    .word reset + 1

    .section .text
reset:
    // GPIO needs no clock or reset release on the nRF52. Pins start as
    // inputs with no pull, so the LEDs are off (no glow as on the RP2040).
    ldr  r0, =P0_BASE
    movs r1, #LED_MASK
    str  r1, [r0, #DIRSET]            // output; OUT bit is 0, so LED on

loop:
    ldr  r2, [r0, #OUT]               // no toggle register: read,
    eors r2, r1                       // flip the LED bit,
    str  r2, [r0, #OUT]               // write back
    ldr  r3, =DELAY
1:  subs r3, #1
    bne  1b
    b    loop

    .ltorg
