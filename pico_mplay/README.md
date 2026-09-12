Zephyr port of ~/devel/pico_mplay (bare-metal Pico SDK Amiga MOD player).

Step 1: confirm a minimal app builds and blinks the LED on both boards
before tackling anything Pico-specific. Two real gaps found already while
scoping this out (see pico_mplay's own CLAUDE.md for the bare-metal side):

- No `led0` devicetree alias exists for rpi_pico/rpi_pico2 in this Zephyr
  tree, so the standard `samples/basic/blinky` DT_ALIAS(led0) pattern needs
  the small per-board overlays in `boards/` here - not a Zephyr limitation,
  just something this board support doesn't provide out of the box.
- PIO has no pioasm/CMake integration in Zephyr: programs must be embedded
  as pre-assembled 16-bit opcode arrays by hand (see the in-tree
  `drivers/led_strip/ws2812_rpi_pico_pio.c` for what that looks like). DMA
  and PWM are fully supported normally. This is why PIO (and therefore I2S,
  which needs a custom PIO transmitter here) is being deferred until basic
  bring-up and DMA/PWM are proven on Zephyr first.

Build (Pico 2, non-W - the /w variant is currently broken, see hello_pico's
README):
```
west build -p -b rpi_pico2/rp2350a/m33 . -S cdc-acm-console -- -DCONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=y
```

Build (original Pico 1 / RP2040):
```
west build -p -b rpi_pico . -S cdc-acm-console -- -DCONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=y
```

Flash:
```
west flash
# or: picotool load -f -x build/zephyr/zephyr.uf2
```
