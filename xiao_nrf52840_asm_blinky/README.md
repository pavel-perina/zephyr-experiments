# XIAO nRF52840 blinky in assembly

Blue user LED (P0.06), no SDK or Zephyr. Uses the assembler and linker
from the Zephyr SDK and `uf2conv.py` from Zephyr.

- `blinky.S`: vector table and the blink loop
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
