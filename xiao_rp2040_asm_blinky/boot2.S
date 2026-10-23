// Second-stage bootloader (boot2). The RP2040 boot ROM copies the first
// 256 bytes of flash to the top of SRAM (0x20041f00), checks the CRC32 in
// the last 4 bytes and jumps here. Our job: set up the flash interface
// (SSI) for execute-in-place, so the CPU can run code directly from flash
// at 0x10000000, then jump to the real program's vector table.
// Same as the Pico SDK's boot2_generic_03h.S: plain 1-bit SPI, READ (03h).

    .cpu cortex-m0plus
    .thumb
    .syntax unified

    .equ XIP_SSI_BASE,   0x18000000
    .equ SSI_CTRLR0,     0x00
    .equ SSI_CTRLR1,     0x04
    .equ SSI_SSIENR,     0x08
    .equ SSI_BAUDR,      0x14
    .equ SSI_SPI_CTRLR0, 0xf4

    // CTRLR0: standard SPI, 32-bit frames, TMOD = EEPROM read (send
    // command + address, then receive)
    .equ CTRLR0_XIP,     (31 << 16) | (3 << 8)
    // SPI_CTRLR0: command 03h, 8-bit command (2 x 4 bits), 24-bit address
    // (6 x 4 bits), command and address both 1-bit
    .equ SPI_CTRLR0_XIP, (0x03 << 24) | (2 << 8) | (6 << 2)

    .equ XIP_BASE,       0x10000000
    .equ VTOR,           0xe000ed08

    .section .text
    .global _stage2_boot
_stage2_boot:
    ldr  r3, =XIP_SSI_BASE
    movs r1, #0
    str  r1, [r3, #SSI_SSIENR]      // disable SSI while configuring
    movs r1, #4
    str  r1, [r3, #SSI_BAUDR]       // SPI clock = clk_sys / 4
    ldr  r1, =CTRLR0_XIP
    str  r1, [r3, #SSI_CTRLR0]
    ldr  r1, =SPI_CTRLR0_XIP
    ldr  r0, =XIP_SSI_BASE + SSI_SPI_CTRLR0   // offset too big for str imm
    str  r1, [r0]
    movs r1, #0
    str  r1, [r3, #SSI_CTRLR1]      // NDF = 0: one 32-bit frame per read
    movs r1, #1
    str  r1, [r3, #SSI_SSIENR]      // enable SSI; flash is now memory-mapped

    // Vector table follows boot2 at XIP_BASE + 0x100: point VTOR at it,
    // load the initial stack pointer and jump to the reset handler.
    ldr  r0, =XIP_BASE + 0x100
    ldr  r1, =VTOR
    str  r0, [r1]
    ldmia r0, {r0, r1}
    msr  msp, r0
    bx   r1

    .ltorg
