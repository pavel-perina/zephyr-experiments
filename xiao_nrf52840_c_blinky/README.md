# XIAO nRF52840 blinky in bare-metal C

Blue user LED (P0.06), timed by the SysTick interrupt. No SDK or Zephyr:
own vector table, startup and linker script, plus headers and
`system_nrf52840.c` (errata workarounds) from nrfx and CMSIS in the west
workspace. Built with the Zephyr SDK's GCC.

- `startup.c`: vector table, copies `.data`, zeroes `.bss`, sets VTOR,
  calls `SystemInit()` and `main()`
- `main.c`: the blink loop
- `link.ld`: application at 0x27000, after the MBR and SoftDevice, so the
  Adafruit UF2 bootloader keeps working

```sh
make          # build/blinky.uf2
make dis      # disassembly
```

Flash: double-tap `reset` to get the XIAO-SENSE drive, then
```sh
cp build/blinky.uf2 /run/media/$USER/XIAO-SENSE/
```
