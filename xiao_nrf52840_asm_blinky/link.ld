/* Application area of the Adafruit nRF52 bootloader layout with
 * SoftDevice S140 v7 (Zephyr: nrf52840_partition_uf2_sdv7.dtsi).
 */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x27000, LENGTH = 0xc5000
}

SECTIONS
{
    .vectors : { KEEP(*(.vectors)) } > FLASH
    .text    : { *(.text*) } > FLASH
}
