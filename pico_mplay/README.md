Zephyr port of ~/devel/pico_mplay (bare-metal Pico SDK Amiga MOD player).

Progress so far:

1. **Blinky** - confirm a minimal app builds and runs on both boards. No
   `led0` devicetree alias exists for rpi_pico/rpi_pico2 in this Zephyr
   tree, so the standard `samples/basic/blinky` DT_ALIAS(led0) pattern
   needed the small per-board overlays in `boards/` here - not a Zephyr
   limitation, just something this board support doesn't provide out of
   the box.
2. **DMA + PWM** (current `src/main.c`) - reproduces the bare-metal
   project's original step 1: a DMA channel streams a slow (~4 Hz,
   LED-visible) sine wave's duty values into PWM slice 4 channel B
   (GPIO25), paced by that slice's own wrap DREQ. Zephyr's `pwm.h` has no
   concept of a DMA-fed duty stream (it's a one-shot "set period+pulse"
   API), so this is hybrid: Zephyr's PWM driver sets up the period once,
   then DMA writes duty values directly into the raw `pwm_hw` register
   struct (same pico-sdk HAL header the Zephyr PWM driver itself uses).
   Zephyr's `dma.h` also has no working "repeat forever" mode on this SoC
   (`dma_config`'s `cyclic` flag isn't implemented by this driver), so -
   same as bare-metal - the DMA channel is manually re-armed from its own
   completion callback (`dma_reload` + `dma_start`). Verified via a boot-time
   printout that `cycles_per_sec`/`period_cycles`/`phase_inc` match the
   bare-metal calculation exactly (150 MHz / 3125 cycles / 4 Hz on Pico 2).

Next: PIO has no pioasm/CMake integration in Zephyr - programs must be
embedded as pre-assembled 16-bit opcode arrays by hand (see the in-tree
`drivers/led_strip/ws2812_rpi_pico_pio.c` for what that looks like). That's
its own dedicated step, deferred until DMA/PWM were proven working here
first. I2S (needing a custom PIO transmitter, same as bare-metal) follows
after that.

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
# or: picotool load -x build/zephyr/zephyr.uf2
```
Note `picotool load -f` (force-reboot a running board into BOOTSEL) does
**not** work against this firmware: Zephyr's CDC ACM stack enumerates under
a Nordic/Zephyr placeholder VID (`2fe3:0004`), not Raspberry Pi's `2e8a`, so
picotool doesn't recognize it as reset-capable. Put the board in BOOTSEL
manually (hold BOOTSEL while plugging in) before flashing.
