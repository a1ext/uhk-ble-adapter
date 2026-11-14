"""BLE link to the paired UHK BLE adapter: the OTA service of src/ota.h -
firmware images, standby animations, animation / calendar commands and the
live packets (spectrum, system load).

Windows: WinRT GATT through Windows' own connection to the paired adapter, so
it does not need to advertise (needs the projections `pip install bleak`
installs). Linux / macOS: ble_link_bleak.py, through bleak.
"""
import asyncio
import atexit
import hashlib
import struct
import sys
import time
import uuid

SERVICE = uuid.UUID("6a1e0100-2b5e-4f8a-9c3d-75ab8e4f0a01")
CTRL = uuid.UUID("6a1e0101-2b5e-4f8a-9c3d-75ab8e4f0a01")
DATA = uuid.UUID("6a1e0102-2b5e-4f8a-9c3d-75ab8e4f0a01")   # also the live packets (src/live.h, src/sysmon.h)
WINDOW = 4 * 1024            # bytes in flight beyond the last ack; 12 KB (~63 KB/s) crashed the adapter
ERASE_TIMEOUT = 60           # s, the adapter erases the target partition first
BEGIN_FIRMWARE, BEGIN_ANIMATION = 0x01, 0x03
ST_RECEIVING, ST_REBOOTING, ST_STORED, ST_INFO, ST_DISMISSED = 0x01, 0x02, 0x03, 0x10, 0x20
ERRORS = {0x81: "begin / erase failed", 0x82: "overflow", 0x83: "flash write failed",
          0x84: "image rejected", 0x85: "SHA-256 mismatch", 0x86: "bad size", 0x87: "aborted",
          0x88: "timeout", 0x89: "not a valid animation file"}
BUSY_HINT = ("the adapter is busy: another program is using it (the live spectrum, "
             "uhk-ble-live.exe / ble_live.py?) - pause or close it, then try again")


class LinkBase:
    """What the tools use. A backend connects and does the two GATT writes;
    everything above that (commands, uploads with flow control, notifications)
    is here."""
    name = "?"
    mtu = 23
    busy = False     # another process holds the service (the live spectrum)

    def __init__(self):
        self.events = asyncio.Queue()
        self.listeners = []          # fn(bytes) for every notification, besides the queue

    def _notified(self, data: bytes):
        """A notification on the control characteristic, on the event loop."""
        self.events.put_nowait(data)
        for fn in list(self.listeners):
            fn(data)

    async def connect(self, name_prefix="UHK60 BLE"):
        raise NotImplementedError

    async def write_ctrl(self, payload: bytes) -> bool:
        raise NotImplementedError

    async def write_data(self, payload: bytes) -> bool:
        raise NotImplementedError

    def close(self):
        pass

    async def set_clock(self):
        """Local time into the adapter's RTC (brightness by the time of day). The RTC has
        a backup cell; refreshing it on every connection fixes drift and DST changes."""
        t = time.localtime()
        await self.command(struct.pack("<BHBBBBB", 0x14, t.tm_year, t.tm_mon, t.tm_mday,
                                       t.tm_hour, t.tm_min, min(t.tm_sec, 59)))

    async def command(self, payload: bytes):
        if not await self.write_ctrl(payload):
            sys.exit(f"command {payload[:1].hex()} failed")

    async def send_live(self, payload: bytes) -> bool:
        """One live packet ("LV" / "SY" ...), write without response on the data
        characteristic - the adapter takes it as such outside a transfer."""
        return await self.write_data(payload)

    async def event(self, timeout):
        """Next notification as raw bytes."""
        return await asyncio.wait_for(self.events.get(), timeout)

    async def state(self, timeout):
        """Next <state u8> <offset u32> notification; exits on an adapter error."""
        while True:
            v = await self.event(timeout)
            if len(v) < 5 or v[0] == ST_INFO:
                continue
            state, offset = struct.unpack("<BI", v[:5])
            if state >= 0x80:
                sys.exit(f"\nadapter error 0x{state:02X}: {ERRORS.get(state, '?')} at {offset}")
            return state, offset

    async def upload(self, payload: bytes, begin_op: int, window=WINDOW, what="image"):
        """Begin, stream with flow control, wait for the final state (2 or 3)."""
        digest = hashlib.sha256(payload).digest()
        print(f"{what}: {len(payload)} bytes, sha256 {digest.hex()[:16]}...")
        await self.command(bytes([begin_op]) + struct.pack("<I", len(payload)) + digest)
        print("erasing...", end="", flush=True)
        try:
            state, offset = await self.state(ERASE_TIMEOUT)
        except asyncio.TimeoutError:
            sys.exit("\nno answer from the adapter")
        print(" done")

        chunk = max(20, self.mtu - 3)
        acked, sent, t0, shown = 0, 0, time.time(), -1
        while acked < len(payload):
            while sent < len(payload) and sent - acked < window:
                part = payload[sent:sent + chunk]
                if not await self.write_data(part):
                    sys.exit(f"\nwrite failed at {sent}")
                sent += len(part)
            try:
                state, offset = await self.state(10)
            except asyncio.TimeoutError:
                sys.exit(f"\nadapter stopped acknowledging at {acked} bytes")
            if state in (ST_REBOOTING, ST_STORED):  # arrives with the last ack
                acked = len(payload)
                break
            acked = offset
            pct = acked * 100 // len(payload)
            if pct != shown:
                shown = pct
                rate = acked / max(time.time() - t0, 0.001) / 1024
                print(f"\r{pct:3d}%  {acked // 1024} / {len(payload) // 1024} KB  {rate:.1f} KB/s",
                      end="", flush=True)

        if state not in (ST_REBOOTING, ST_STORED):  # wait for the verification result
            try:
                while state not in (ST_REBOOTING, ST_STORED):
                    state, offset = await self.state(30)
            except asyncio.TimeoutError:
                sys.exit("\nno confirmation from the adapter after the last byte")
        print(f"\nverified in {time.time() - t0:.0f} s")
        return state


# ---------------- Windows: WinRT ----------------
if sys.platform == "win32":
    from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothLEDevice
    from winrt.windows.devices.bluetooth.genericattributeprofile import (
        GattClientCharacteristicConfigurationDescriptorValue as Cccd, GattCommunicationStatus,
        GattSession, GattWriteOption)
    from winrt.windows.devices.enumeration import DeviceInformation
    from winrt.windows.storage.streams import DataReader, DataWriter

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

    class WinRtLink(LinkBase):
        """Control + data characteristics of the adapter's OTA service, through
        Windows' own connection to the paired adapter."""

        async def connect(self, name_prefix="UHK60 BLE"):
            try:
                return await self._connect(name_prefix)
            except BaseException:            # sys.exit() too: a session / service left open here keeps
                self.close()                 # every later attempt of this process at ACCESS_DENIED
                raise

        async def _connect(self, name_prefix):
            devices = await find_adapter(name_prefix)
            if not devices:
                sys.exit(f"no paired device named '{name_prefix}*' - pair the adapter first")
            dev = devices[0]
            print(f"device: {dev.name} ({'connected' if dev.connection_status == 1 else 'not connected'})")
            self.device, self.service, self.name = dev, None, dev.name
            atexit.register(self.close)
            self.session = await GattSession.from_device_id_async(dev.bluetooth_device_id)
            self.session.maintain_connection = True
            # right after another process' session Windows answers ACCESS_DENIED for a moment, and
            # while it re-reads the services after a firmware change a call may even throw: retry
            for attempt in range(13):        # a killed tool's service stays busy up to ~20 s
                if attempt == 2:
                    print("waiting for Windows to release the adapter's service...")
                if attempt:
                    await asyncio.sleep(2)
                try:
                    res = await dev.get_gatt_services_for_uuid_with_cache_mode_async(SERVICE, BluetoothCacheMode.UNCACHED)
                    if res.status != GattCommunicationStatus.SUCCESS or not res.services:
                        self.busy |= res.status == GattCommunicationStatus.ACCESS_DENIED
                        what = f"OTA service not found (status {res.status})"
                        continue
                    self.close_service()
                    self.service = res.services[0]
                    chars = {}
                    for u in (CTRL, DATA):
                        chars[u] = await self._characteristic(u)
                except OSError as e:
                    what = f"GATT discovery failed ({e})"
                    continue
                except LookupError as e:
                    what = str(e)
                    continue
                break
            else:
                if self.busy:
                    sys.exit(BUSY_HINT)
                sys.exit(what + " - adapter firmware without OTA, or Windows cached an old service "
                         "list: remove and re-pair the adapter")
            self.ctrl, self.data = chars[CTRL], chars[DATA]
            self.mtu = self.session.max_pdu_size

            loop = asyncio.get_running_loop()

            def on_notify(_, args):
                loop.call_soon_threadsafe(self._notified, buffer_bytes(args.characteristic_value))

            self.ctrl.add_value_changed(on_notify)
            st = await self.ctrl.write_client_characteristic_configuration_descriptor_async(Cccd.NOTIFY)
            if st != GattCommunicationStatus.SUCCESS:
                sys.exit(f"cannot subscribe to OTA notifications (status {st})")
            await self.set_clock()
            return self

        async def _characteristic(self, u):
            """From the device; Windows' cache when that fails (it does after the attribute
            table of a bonded device changed - the handles we use stayed the same)."""
            for mode in (BluetoothCacheMode.UNCACHED, BluetoothCacheMode.CACHED):
                try:
                    cr = await self.service.get_characteristics_for_uuid_with_cache_mode_async(u, mode)
                except OSError:
                    continue
                if cr.status == GattCommunicationStatus.SUCCESS and cr.characteristics:
                    return cr.characteristics[0]
                self.busy |= cr.status == GattCommunicationStatus.ACCESS_DENIED   # another process has it
            raise LookupError(f"OTA characteristic {u} not found")

        def close_service(self):
            if self.service:
                self.service.close()
                self.service = None

        def close(self):
            """Release the service: an open one keeps the next process at ACCESS_DENIED for a while."""
            atexit.unregister(self.close)    # it held this object (and its WinRT handles) for good
            self.close_service()
            for o in ("session", "device"):
                if getattr(self, o, None):
                    getattr(self, o).close()
                    setattr(self, o, None)

        async def write_ctrl(self, payload: bytes) -> bool:
            st = await self.ctrl.write_value_with_option_async(buffer(payload), GattWriteOption.WRITE_WITH_RESPONSE)
            return st == GattCommunicationStatus.SUCCESS

        async def write_data(self, payload: bytes) -> bool:
            st = await self.data.write_value_with_option_async(buffer(payload), GattWriteOption.WRITE_WITHOUT_RESPONSE)
            return st == GattCommunicationStatus.SUCCESS

    Link = WinRtLink
else:
    from ble_link_bleak import BleakLink as Link  # noqa: F401
