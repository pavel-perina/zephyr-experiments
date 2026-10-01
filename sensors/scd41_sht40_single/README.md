# SCD41 + SHT40 single-shot logger

**Board:** XIAO BLE (nRF52840) · `west build -b xiao_ble` · SCD41 and SHT40
on the XIAO I2C pins (D4 = SDA, D5 = SCL, 3.3 V, GND).

Minimal first step towards a low-power CO2 logger: a thread measures every
30 s - the SCD41 in **single-shot** mode (~5 s per measurement) and the
SHT40 - and prints to the USB console, which also runs a shell:

- `logger now` - measure immediately
- `logger interval <s>` - set the interval (6..3600 s)
- `logger toffset [C]` - show / set the temperature offset the app applies

The SCD41's stored temperature offset (factory 4 C, meant to cancel
self-heating in periodic mode) over-corrects in single-shot mode: the first
run read ~4.3 C below the SHT40 and a much higher %RH. Changing it in the
sensor doesn't stick with Zephyr's driver in single-shot mode (it powers the
sensor down right after setting it, and wake_up reloads the stored value),
so the app compensates in software: T += stored - wanted offset, and RH is
recomputed for the corrected temperature with the Magnus formula. Lines
show the corrected values and the raw ones in brackets.
- `sensor get scd41@62` / `sensor get sht4x@44` - Zephyr's sensor shell

No deep sleep yet: Zephyr's idle already sleeps between measurements; the
next steps are the SCD41's power_down/wake_up (device PM) and measuring the
current. The SCD40 has no single-shot mode - see boards/xiao_ble.overlay.
