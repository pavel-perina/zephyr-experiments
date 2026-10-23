/* Flash image layout: 256 bytes of boot2, then the vector table at
 * 0x10000100 (where boot2 points VTOR), then code.
 */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x10000000, LENGTH = 2M
}

SECTIONS
{
    .boot2   : { KEEP(*(.boot2)) } > FLASH
    .vectors : { KEEP(*(.vectors)) } > FLASH
    .text    : { *(.text*) } > FLASH
}
