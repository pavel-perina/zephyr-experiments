#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["bleak"]
# ///
"""Log the veml7700 BLE beacon to a JSONL file.

Writes one line per sample to <start datetime>-veml7700-data.jsonl, e.g.
2026-10-28_12-46-13-veml7700-data.jsonl, in the current directory (or
--dir). Each sample is advertised ~10 times; repeats (same device uptime)
are dropped. Stop with Ctrl-C.

    {"time": "2026-10-28T12:46:23.512+01:00", "uptime": 120, "als": 5170,
     "white": 11761, "range": 4, "gain": "1", "it_ms": 100, "rssi": -60}

"time" is the host clock at first reception (up to ~1 s after the
measurement), "uptime" the XIAO's uptime [s] when it measured. A reset of
the XIAO shows as uptime going down. Raw counts only; lux = als * res of
the range (see RANGES), plus the correction for gain 1/8 and 1/4.
"""
import argparse
import asyncio
import json
import struct
from datetime import datetime
from pathlib import Path

from bleak import BleakScanner

COMPANY_ID = 0xFFFF
FORMAT_VERSION = 1

# Index = range byte in the beacon; must match ranges[] in src/main.c.
RANGES = [
    ("1/8", 25, 1.8432),
    ("1/8", 50, 0.9216),
    ("1/8", 100, 0.4608),
    ("1/4", 100, 0.2304),
    ("1", 100, 0.0576),
    ("2", 100, 0.0288),
    ("2", 200, 0.0144),
    ("2", 400, 0.0072),
]


def parse(payload):
    """Manufacturer data after the company ID -> dict, or None."""
    if len(payload) != 10 or payload[0] != FORMAT_VERSION:
        return None
    _, uptime, als, white, rng = struct.unpack("<BIHHB", payload)
    sample = {"uptime": uptime, "als": als, "white": white, "range": rng}
    if rng < len(RANGES):
        sample["gain"], sample["it_ms"], _ = RANGES[rng]
    else:
        sample["error"] = "fetch failed"
    return sample


async def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dir", type=Path, default=Path("."), help="output directory")
    ap.add_argument("--name", default="veml7700", help="advertised device name")
    args = ap.parse_args()

    start = datetime.now()
    path = args.dir / f"{start:%Y-%m-%d_%H-%M-%S}-veml7700-data.jsonl"
    last_uptime = None

    with open(path, "a", buffering=1) as out:  # line-buffered: survives a kill
        def on_adv(device, adv):
            nonlocal last_uptime
            if adv.local_name not in (None, args.name):
                return
            payload = adv.manufacturer_data.get(COMPANY_ID)
            sample = parse(payload) if payload else None
            if sample is None or sample["uptime"] == last_uptime:
                return
            last_uptime = sample["uptime"]
            line = {"time": datetime.now().astimezone().isoformat(timespec="milliseconds")}
            line.update(sample)
            line["rssi"] = adv.rssi
            out.write(json.dumps(line) + "\n")
            print(json.dumps(line), flush=True)

        print(f"logging to {path}, Ctrl-C to stop", flush=True)
        async with BleakScanner(on_adv):
            await asyncio.Event().wait()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
