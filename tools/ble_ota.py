"""Firmware update of the UHK BLE adapter over Bluetooth (Windows).

Uses Windows' own connection to the paired adapter (WinRT GATT), so the
adapter does not need to advertise. Protocol: see src/ota.h.

    python tools/ble_ota.py .pio/build/t-display-s3-amoled-plus/firmware.bin
    pio run -t ota            (same, via tools/pio_extra.py)

Needs the WinRT Python projections, e.g. `pip install bleak` (pulls them in).
"""
import argparse
import asyncio
import hashlib
import struct
import sys
import time
import uuid

from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothLEDevice
from winrt.windows.devices.bluetooth.genericattributeprofile import (
    GattClientCharacteristicConfigurationDescriptorValue as Cccd, GattCommunicationStatus,
    GattSession, GattWriteOption)
from winrt.windows.devices.enumeration import DeviceInformation
from winrt.windows.storage.streams import DataReader, DataWriter

SERVICE = uuid.UUID("6a1e0100-2b5e-4f8a-9c3d-75ab8e4f0a01")
CTRL = uuid.UUID("6a1e0101-2b5e-4f8a-9c3d-75ab8e4f0a01")
DATA = uuid.UUID("6a1e0102-2b5e-4f8a-9c3d-75ab8e4f0a01")
WINDOW = 12 * 1024           # bytes in flight beyond the last ack (adapter buffers 16 KB)
ERASE_TIMEOUT = 60           # s, the adapter erases the target partition first
ERRORS = {0x81: "begin failed", 0x82: "overflow", 0x83: "flash write failed", 0x84: "image rejected",
          0x85: "SHA-256 mismatch", 0x86: "bad size", 0x87: "aborted", 0x88: "timeout"}


def buffer(data: bytes):
    w = DataWriter()
    w.write_bytes(data)
    return w.detach_buffer()


def buffer_bytes(buf) -> bytes:
    r = DataReader.from_buffer(buf)
    b = bytearray(r.unconsumed_buffer_length)
    r.read_bytes(b)
    return bytes(b)


async def find_adapter(prefix: str):
    """Paired LE devices named <prefix>*, the connected one first."""
    sel = BluetoothLEDevice.get_device_selector_from_pairing_state(True)
    found = []
    for info in await DeviceInformation.find_all_async_aqs_filter(sel):
        if info.name.startswith(prefix):
            dev = await BluetoothLEDevice.from_id_async(info.id)
            if dev:
                found.append(dev)
    found.sort(key=lambda d: d.connection_status != 1)
    return found


async def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("firmware")
    ap.add_argument("--name", default="UHK60 BLE", help="device name prefix")
    args = ap.parse_args()

    image = open(args.firmware, "rb").read()
    digest = hashlib.sha256(image).digest()
    print(f"{args.firmware}: {len(image)} bytes, sha256 {digest.hex()[:16]}...")

    devices = await find_adapter(args.name)
    if not devices:
        sys.exit(f"no paired device named '{args.name}*' - pair the adapter first")
    dev = devices[0]
    print(f"device: {dev.name} ({'connected' if dev.connection_status == 1 else 'not connected'})")

    session = await GattSession.from_device_id_async(dev.bluetooth_device_id)
    session.maintain_connection = True
    res = await dev.get_gatt_services_for_uuid_with_cache_mode_async(SERVICE, BluetoothCacheMode.UNCACHED)
    if res.status != GattCommunicationStatus.SUCCESS or not res.services:
        sys.exit(f"OTA service not found (status {res.status}) - adapter firmware without OTA, "
                 "or Windows cached an old service list: remove and re-pair the adapter")
    svc = res.services[0]
    chars = {}
    for u in (CTRL, DATA):
        cr = await svc.get_characteristics_for_uuid_with_cache_mode_async(u, BluetoothCacheMode.UNCACHED)
        if cr.status != GattCommunicationStatus.SUCCESS or not cr.characteristics:
            sys.exit(f"OTA characteristic {u} not found (status {cr.status})")
        chars[u] = cr.characteristics[0]
    ctrl, data = chars[CTRL], chars[DATA]

    loop = asyncio.get_running_loop()
    events = asyncio.Queue()

    def on_notify(_, args):
        v = buffer_bytes(args.characteristic_value)
        if len(v) >= 5:
            loop.call_soon_threadsafe(events.put_nowait, struct.unpack("<BI", v[:5]))

    ctrl.add_value_changed(on_notify)
    st = await ctrl.write_client_characteristic_configuration_descriptor_async(Cccd.NOTIFY)
    if st != GattCommunicationStatus.SUCCESS:
        sys.exit(f"cannot subscribe to OTA notifications (status {st})")

    def check(state, offset):
        if state >= 0x80:
            sys.exit(f"\nadapter error 0x{state:02X}: {ERRORS.get(state, '?')} at {offset}")

    st = await ctrl.write_value_with_option_async(
        buffer(bytes([0x01]) + struct.pack("<I", len(image)) + digest), GattWriteOption.WRITE_WITH_RESPONSE)
    if st != GattCommunicationStatus.SUCCESS:
        sys.exit(f"begin command failed (status {st})")
    print("erasing the update partition...", end="", flush=True)
    try:
        state, offset = await asyncio.wait_for(events.get(), ERASE_TIMEOUT)
    except asyncio.TimeoutError:
        sys.exit("\nno answer from the adapter")
    check(state, offset)
    print(" done")

    chunk = max(20, session.max_pdu_size - 3)
    acked, sent, t0, shown = 0, 0, time.time(), -1
    while acked < len(image):
        while sent < len(image) and sent - acked < WINDOW:
            part = image[sent:sent + chunk]
            st = await data.write_value_with_option_async(buffer(part), GattWriteOption.WRITE_WITHOUT_RESPONSE)
            if st != GattCommunicationStatus.SUCCESS:
                sys.exit(f"\nwrite failed at {sent} (status {st})")
            sent += len(part)
        try:
            state, offset = await asyncio.wait_for(events.get(), 10)
        except asyncio.TimeoutError:
            sys.exit(f"\nadapter stopped acknowledging at {acked} bytes")
        check(state, offset)
        if state == 0x02:          # done arrives with the last ack
            acked = len(image)
            break
        acked = offset
        pct = acked * 100 // len(image)
        if pct != shown:
            shown = pct
            rate = acked / max(time.time() - t0, 0.001) / 1024
            print(f"\r{pct:3d}%  {acked // 1024} / {len(image) // 1024} KB  {rate:.1f} KB/s", end="", flush=True)

    if state != 0x02:              # wait for verify + reboot notice
        try:
            while True:
                state, offset = await asyncio.wait_for(events.get(), 30)
                check(state, offset)
                if state == 0x02:
                    break
        except asyncio.TimeoutError:
            sys.exit("\nno confirmation from the adapter after the last byte")
    print(f"\nimage verified in {time.time() - t0:.0f} s - the adapter reboots into the new firmware")
    print("it confirms the update once Windows reconnects; a crash before that rolls it back")


asyncio.run(main())
