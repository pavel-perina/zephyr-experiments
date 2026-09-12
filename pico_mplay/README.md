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

3. **PIO + DMA I2S** (current `src/main.c`) - real audio to a MAX98357A
   speaker via the bare-metal project's `i2s_out.pio` transmitter, reused
   as-is (not reimplemented): its pioasm-compiled instruction bytes are
   lifted straight from that project's generated `build/i2s_out.pio.h` into
   `src/i2s_out_pio.h` here via Zephyr's `RPI_PICO_PIO_DEFINE_PROGRAM`
   macro - see that header's comment for why (Zephyr has no pioasm/CMake
   integration; programs must be embedded as pre-assembled 16-bit opcode
   arrays by hand, same as the in-tree `ws2812_rpi_pico_pio.c` driver does).
   Confirmed there's **no pure-Zephyr path for PIO**: `pio_rpi_pico` only
   allocates state machines and hands back a raw `PIO` handle - loading the
   program, configuring side-set/pins/clkdiv, and driving DMA into the TX
   FIFO are all the same pico-sdk calls the bare-metal version uses. One
   real gotcha hit and fixed: pico-sdk's `hardware/pio.h` `#define`s
   `pio0`/`pio1` as its own singletons, which collides with Zephyr's
   `DT_NODELABEL(pio0)` macro - fixed with `#undef pio0`/`#undef pio1`
   right after including that header (see the comment in `main.c`).
   Verified playing the same logarithmic sweep test tone as the bare-metal
   I2S bring-up, audibly, on real Pico 1 (RP2040) hardware.

4. **Full MOD playback** (current `src/main.c`) - `mod_player.c`/`.h`
   copied over unmodified (they had zero pico-sdk/Zephyr dependency
   already, just standard C) replace the sweep generator as the sample
   source; `fill_buffer()` calls `mod_player_produce()` instead, same
   packing into stereo I2S frames as before. The MOD file (`AXEL_F.MOD`,
   checked into the repo) is embedded via the same CMake
   `add_custom_command` + `xxd -i` approach as the bare-metal project,
   generating `mod_data.h` into the build directory rather than committing
   a much larger generated header - see `CMakeLists.txt`. Built and linked
   on the first try; no picolibc heap/malloc issues surfaced. Verified
   playing correctly (position/row advancing through multiple patterns)
   and audibly, on real Pico 1 (RP2040) hardware - full parity with the
   bare-metal player achieved.

## No standalone Pico SDK needed

Every `hardware/*.h` header used above (`hardware/pio.h`, `hardware/clocks.h`,
`hardware/structs/pwm.h`, ...) comes from `hal_rpi_pico`, a Zephyr-managed
module fetched by `west update` per this workspace's manifest - a separate
checkout from `~/.pico-sdk` (the standalone Pico SDK install used by the
bare-metal `pico_mplay` project), not a dependency on it. Cloning this
workspace and running `west update` is enough; the standalone SDK is never
touched. The include paths aren't set up by this app's `CMakeLists.txt`
either - they're wired in globally by Zephyr's own RP2040/2350 support,
since Zephyr's in-tree drivers (`pwm_rpi_pico.c`, `dma_rpi_pico.c`,
`pio_rpi_pico.c`) need exactly the same headers, confirmed by checking the
actual `-I` flags in `build/compile_commands.json`. Worth noting: since the
two SDK checkouts are tracked completely independently, they could in
principle drift apart over time (different pinned versions/patches) even
though they're the same lineage - it happened to work seamlessly here
(the bare-metal build's pioasm-compiled PIO bytes ran correctly against
this workspace's headers/register offsets), but that's not a guarantee
that holds forever.

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
