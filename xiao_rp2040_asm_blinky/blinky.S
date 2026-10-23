// Blink the XIAO RP2040's blue user LED (GPIO25, active low) - no SDK,
// no Zephyr, just registers. Clocks are left as the boot ROM set them
// (clk_sys from the ring oscillator, roughly 6 MHz), so the delay is
// approximate.

    .cpu cortex-m0plus
    .thumb
    .syntax unified

    .equ RESETS_BASE,       0x4000c000
    .equ RESETS_CLR,        RESETS_BASE + 0x3000  // atomic bit-clear alias
    .equ RESET_DONE,        0x08
    .equ RESET_IO_BANK0,    1 << 5
    .equ RESET_PADS_BANK0,  1 << 8

    .equ IO_BANK0_BASE,     0x40014000
    .equ GPIO25_CTRL,       IO_BANK0_BASE + 0xcc
    .equ FUNCSEL_SIO,       5

    .equ SIO_BASE,          0xd0000000
    .equ GPIO_OUT_XOR,      0x1c
    .equ GPIO_OE_SET,       0x24

    .equ LED_MASK,          1 << 25
    .equ DELAY,             1000000   // loop is 3 cycles: ~0.5 s at 6 MHz

    // Vector table: initial SP, then reset handler (bit 0 set = Thumb).
    // Other exceptions aren't used; a fault would jump to address 0 (ROM).
    .section .vectors, "a"
    .word 0x20042000                  // top of SRAM
    .word reset + 1

    .section .text
reset:
    // GPIO blocks start held in reset; release them and wait until done.
    ldr  r0, =RESETS_CLR
    ldr  r1, =RESET_IO_BANK0 | RESET_PADS_BANK0
    str  r1, [r0]
    ldr  r0, =RESETS_BASE
1:  ldr  r2, [r0, #RESET_DONE]
    ands r2, r1
    cmp  r2, r1
    bne  1b

    // GPIO25 pin function = SIO (software-controlled GPIO)
    ldr  r0, =GPIO25_CTRL
    movs r1, #FUNCSEL_SIO
    str  r1, [r0]

    // Output enable. OUT starts at 0, so the (active-low) LED is on.
    ldr  r0, =SIO_BASE
    ldr  r1, =LED_MASK
    str  r1, [r0, #GPIO_OE_SET]

loop:
    str  r1, [r0, #GPIO_OUT_XOR]      // toggle
    ldr  r2, =DELAY
2:  subs r2, #1                       // 1 cycle
    bne  2b                           // 2 cycles when taken
    b    loop

    .ltorg
