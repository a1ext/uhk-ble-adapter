"""Firmware update of the UHK BLE adapter over Bluetooth (Windows).

Uses Windows' own connection to the paired adapter (WinRT GATT), so the
adapter does not need to advertise. Protocol: see src/ota.h.

    python tools/ble_ota.py .pio/build/t-display-s3-amoled-plus/firmware.bin
    pio run -t ota            (same, via tools/pio_extra.py)

Needs the WinRT Python projections, e.g. `pip install bleak` (pulls them in).
"""
import argparse
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ble_link import BEGIN_FIRMWARE, WINDOW, Link  # noqa: E402


async def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("firmware")
    ap.add_argument("--name", default="UHK60 BLE", help="device name prefix")
    ap.add_argument("--window", type=int, default=WINDOW,
                    help="bytes in flight beyond the last ack (smaller = slower, gentler)")
    args = ap.parse_args()

    image = open(args.firmware, "rb").read()
    link = await Link().connect(args.name)
    await link.upload(image, BEGIN_FIRMWARE, args.window, what=args.firmware)
    print("the adapter reboots into the new firmware")
    print("it confirms the update once Windows reconnects; a crash before that rolls it back")


asyncio.run(main())
