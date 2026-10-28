# VEML7700 ambient light

Adafruit VEML7700 breakout, read every 10 s, printed to the console and
sent in a BLE beacon. `tools/ble_logger.py` logs the beacon to a JSONL file.
Wiring per board in `boards/<board>.overlay`.

| Board | SDA | SCL | Console | Flash |
|---|---|---|---|---|
| XIAO BLE (nRF52840) | D4 | D5 | USB CDC | `west flash -r uf2` |

XIAO BLE (UF2 bootloader: double-tap reset, drive `XIAO-SENSE`):

```
west build -p -b xiao_ble
west flash -r uf2          # drive must be mounted
picocom /dev/ttyACM0
```

Output format:

```
[12 s] 187.452 lx (uncorrected 187.330), ALS 813, white 1850, range 1/4 100 ms
```

**Auto-ranging** (after Vishay's application note 84323, "Designing the
VEML7700 Into an Application"): start at gain 1/8, 100 ms. The app keeps the
ALS counts between 1000 and 10000 by picking the most sensitive range
where the same light gives at most 10000 counts. Going up in sensitivity
it raises the gain first (1/8, 1/4, 1, 2), then the integration time.
Going down it ends at gain 1/8, 25 ms (~120 klx raw). If a reading
saturates (65535), the app goes straight to the least sensitive range and
measures again.
400 ms is the longest integration time used. The driver measures
single-shot and waits 2x the integration time, so 800 ms would take 1.6 s
and break the 1 Hz period.

**Correction**: above ~1000 lx the sensor's response is not linear. For
gain 1/8 and 1/4 the note gives
`lux = 6.0135e-13 x^4 - 9.3924e-9 x^3 + 8.1488e-5 x^2 + 1.0023 x`, with x the
uncorrected lux (counts x resolution). The app applies it only at
those gains and prints both values. Gain 1 and 2 are only used below
~580 lx, where the response is linear.

Lux is computed in the app from the raw counts; the driver's own lux
value is whole lux only.

## BLE beacon and logger

Non-connectable advertising every ~1 s, name `veml7700`, manufacturer data
with company ID 0xFFFF (reserved for testing): format version, uptime of
the sample [s], ALS and white counts, range index. Byte layout in
`advertise()` in `src/main.c`. Each 10 s sample is sent ~10 times.

```
tools/ble_logger.py --dir ~/data      # Ctrl-C stops
```

It's a [uv](https://docs.astral.sh/uv/) script: bleak is declared inline and
uv installs it into its cache on the first run (or `uv run
tools/ble_logger.py`). On Linux bleak talks to BlueZ over D-Bus, so
`bluetoothd` must be running; no root needed.

Writes `<start>-veml7700-data.jsonl`, e.g.
`2026-10-28_12-46-13-veml7700-data.jsonl`, one line per sample:

```
{"time": "2026-10-28T12:51:01.254+01:00", "uptime": 10, "als": 7991, "white": 19976, "range": 6, "gain": "2", "it_ms": 200, "rssi": -57}
```

`time` is the host clock at first reception, `uptime` the XIAO's when it
measured (a reset shows as uptime going down). Missing samples show as
gaps in `uptime`. The range table is in `RANGES` in the script; it must
match `ranges[]` in `src/main.c`.

## Comparing ranges: `als sweep`

The console also runs a shell. `als sweep` measures the same light in
every range (4 fetches each) and prints the counts and the mean lux, so
the ranges can be compared. Keep the light steady while it runs (~8 s).

First run, room light (2026-10-28):

```
1/8 25 ms     64    64    64    64  117.965 (119.355)
1/8 50 ms    129   128   128   129  118.426 (119.825)
1/8 100 ms   258   258   258   259  119.002 (120.414)
1/4 100 ms   520   521   519   516  119.578 (121.002)
1 100 ms    1818  1814  1817  1817  104.630
2 100 ms    3634  3642  3635  3633  104.717
2 200 ms    7272  7262  7257  7272  104.627
2 400 ms   14526 14510 14502 14504  104.476
```

Integration time scales exactly (x2 per step), and so do gain 1/8 -> 1/4
and gain 1 -> 2. Gain 1/4 -> 1 gives x3.5 instead of x4, so gains 1/8 and
1/4 read ~14 % more than gains 1 and 2. Auto-ranging crosses that boundary
at ~230 lx. Which side is right needs a reference meter.
