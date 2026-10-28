"""BLE link to the paired UHK BLE adapter on Linux (BlueZ) and macOS
(CoreBluetooth) through bleak: the same OTA service as ble_link.py, which picks
this backend off Windows.

The adapter does not advertise while its host is connected, so a scan does not
find it then. Linux: BlueZ knows the paired device, the link looks it up with
`bluetoothctl devices Paired` and connects by address. macOS: bleak needs the
peripheral's identifier from a scan - run a tool while the adapter is not
connected (its slot waiting for the Mac), the first run prints the identifier
to pass with --address from then on.
"""
import asyncio
import subprocess
import sys

from bleak import BleakClient, BleakScanner

from ble_link import CTRL, DATA, LinkBase

SCAN_S = 8


class BleakLink(LinkBase):
    address = None     # the tools set it from --address

    async def connect(self, name_prefix="UHK60 BLE"):
        address = self.address or await self._find(name_prefix)
        if not address:
            sys.exit(f"no paired device named '{name_prefix}*': pair the adapter first "
                     "(Linux: bluetoothctl), or pass its address with --address")
        self.client = BleakClient(address, timeout=20)
        try:
            await self.client.connect()
        except Exception as e:  # bleak raises its own and OS errors
            sys.exit(f"cannot connect to {address}: {e}")
        self.mtu = self.client.mtu_size
        if self.name == "?":
            self.name = address
        loop = asyncio.get_running_loop()

        def on_notify(_, data):
            loop.call_soon_threadsafe(self._notified, bytes(data))

        try:
            await self.client.start_notify(str(CTRL), on_notify)
        except Exception as e:
            sys.exit(f"OTA service not found ({e}): adapter firmware without OTA, or not paired "
                     "with the passkey (the service needs an encrypted link)")
        print(f"device: {self.name} (connected, MTU {self.mtu})")
        await self.set_clock()
        return self

    async def _find(self, prefix):
        if sys.platform.startswith("linux"):
            for cmd in (["bluetoothctl", "devices", "Paired"], ["bluetoothctl", "paired-devices"]):
                try:
                    out = subprocess.run(cmd, capture_output=True, text=True, timeout=10).stdout
                except (OSError, subprocess.TimeoutExpired):
                    break
                for line in out.splitlines():         # "Device AA:BB:CC:DD:EE:FF UHK60 BLE 3"
                    parts = line.strip().split(" ", 2)
                    if len(parts) == 3 and parts[0] == "Device" and parts[2].startswith(prefix):
                        self.name = parts[2]
                        return parts[1]
                if out.strip():
                    break
        print(f"scanning {SCAN_S} s (the adapter only advertises while its host is away)...")
        for dev in await BleakScanner.discover(timeout=SCAN_S):
            if (dev.name or "").startswith(prefix):
                self.name = dev.name
                print(f"found {dev.name} at {dev.address} - pass --address {dev.address} next time")
                return dev.address
        return None

    async def write_ctrl(self, payload: bytes) -> bool:
        try:
            await self.client.write_gatt_char(str(CTRL), payload, response=True)
            return True
        except Exception as e:
            print(f"write failed: {e}", file=sys.stderr)
            return False

    async def write_data(self, payload: bytes) -> bool:
        try:
            await self.client.write_gatt_char(str(DATA), payload, response=False)
            return True
        except Exception:
            return False

    def close(self):
        client = getattr(self, "client", None)
        if not client or not client.is_connected:
            return
        try:
            asyncio.get_running_loop().create_task(client.disconnect())
        except RuntimeError:                       # no loop running any more
            asyncio.run(client.disconnect())
