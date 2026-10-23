/* Application area of the Adafruit nRF52 bootloader layout with
 * SoftDevice S140 v7 (Zephyr: nrf52840_partition_uf2_sdv7.dtsi). The
 * SoftDevice isn't enabled, so all of RAM is ours.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x27000,    LENGTH = 0xc5000
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
    } > FLASH

    /* Runs from RAM, stored in flash after .text */
    .data : {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH
    _sidata = LOADADDR(.data);

    .bss (NOLOAD) : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    _estack = ORIGIN(RAM) + LENGTH(RAM);
}
