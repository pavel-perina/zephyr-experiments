# XIAO RP2040 blinky in assembly

Blue user LED (GPIO25), no SDK or Zephyr. Uses the assembler and linker
from the Zephyr SDK, plus `pad_checksum` (Pico HAL) and `uf2conv.py`
(Zephyr) from the west workspace.

- `boot2.S`: second-stage bootloader, sets up flash execute-in-place
- `blinky.S`: vector table and the blink loop
- `link.ld`: boot2 at 0x10000000, vector table at 0x10000100

```sh
make          # build/blinky.uf2
make dis      # disassembly
```

Flash: hold `boot`, press `reset`, then
```sh
udisksctl mount -b /dev/sdX1 && cp build/blinky.uf2 /run/media/$USER/RPI-RP2/
```
or `picotool load -x build/blinky.uf2`.
