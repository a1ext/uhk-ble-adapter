"""The computer's load for the adapter's dashboard (src/sysmon.h): one "SY"
packet per sample - CPU / RAM / GPU / VRAM in percent, network and disk rates,
CPU / GPU temperatures. psutil for the system, NVML (pynvml) for an NVIDIA GPU
on Windows / Linux, IOKit for the GPU of an Apple Silicon Mac; whatever is not
available goes out as 0xFF.
"""
import struct
import sys
import time

import psutil

NA8, NA16 = 0xFF, 0xFFFF


class GpuNvml:
    """NVIDIA: utilization, VRAM in use, temperature."""

    def __init__(self):
        import pynvml
        self.nv = pynvml
        pynvml.nvmlInit()
        self.h = pynvml.nvmlDeviceGetHandleByIndex(0)
        name = pynvml.nvmlDeviceGetName(self.h)
        self.name = name.decode() if isinstance(name, bytes) else name

    def read(self):
        util = self.nv.nvmlDeviceGetUtilizationRates(self.h).gpu
        mem = self.nv.nvmlDeviceGetMemoryInfo(self.h)
        vram = round(mem.used * 100 / mem.total) if mem.total else NA8
        try:
            temp = self.nv.nvmlDeviceGetTemperature(self.h, self.nv.NVML_TEMPERATURE_GPU)
        except self.nv.NVMLError:
            temp = NA8
        return util, vram, temp


class GpuMac:
    """Apple GPU: IOAccelerator's PerformanceStatistics through IOKit (no root, no subprocess)."""
    name = "Apple GPU"

    def __init__(self):
        if sys.platform != "darwin":
            raise OSError("not a Mac")
        import ctypes
        import objc
        self._ctypes = ctypes
        self._objc = objc
        self._iokit = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/IOKit.framework/IOKit")
        self._cf = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        self._iokit.IOServiceMatching.argtypes = [ctypes.c_char_p]
        self._iokit.IOServiceMatching.restype = ctypes.c_void_p
        self._iokit.IOServiceGetMatchingServices.argtypes = [
            ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)
        ]
        self._iokit.IOServiceGetMatchingServices.restype = ctypes.c_int32
        self._iokit.IOIteratorNext.argtypes = [ctypes.c_uint32]
        self._iokit.IOIteratorNext.restype = ctypes.c_uint32
        self._iokit.IORegistryEntryCreateCFProperties.argtypes = [
            ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_uint32
        ]
        self._iokit.IORegistryEntryCreateCFProperties.restype = ctypes.c_int32
        self._iokit.IOObjectRelease.argtypes = [ctypes.c_uint32]
        self._cf.CFRelease.argtypes = [ctypes.c_void_p]

        it = ctypes.c_uint32(0)
        self._iokit.IOServiceGetMatchingServices(
            0, self._iokit.IOServiceMatching(b"IOAccelerator"), ctypes.byref(it)
        )
        self._entry = 0
        while True:
            entry = self._iokit.IOIteratorNext(it.value)
            if not entry:
                break
            if self._read_entry(entry) is not None:
                self._entry = entry
                break
            self._iokit.IOObjectRelease(entry)
        self._iokit.IOObjectRelease(it.value)
        if not self._entry:
            raise LookupError("no IOAccelerator with Device Utilization %")

    def _read_entry(self, entry):
        props = self._ctypes.c_void_p(0)
        if self._iokit.IORegistryEntryCreateCFProperties(entry, self._ctypes.byref(props), None, 0) != 0 or not props.value:
            return None
        try:
            d = self._objc.objc_object(c_void_p=props)
            stats = d.get("PerformanceStatistics")
            if stats and "Device Utilization %" in stats:
                return int(stats["Device Utilization %"])
            return None
        finally:
            self._cf.CFRelease(props)

    def read(self):
        v = self._read_entry(self._entry)
        return (v if v is not None else NA8), NA8, NA8


def pick_gpu():
    for cls in (GpuNvml, GpuMac):
        try:
            return cls()
        except Exception:
            continue
    return None


class Sampler:
    def __init__(self):
        self.gpu = pick_gpu()
        psutil.cpu_percent(None)           # primes the per-call average
        self.t = time.time()
        self.net = psutil.net_io_counters()
        self.disk = psutil.disk_io_counters()
        print(f"system stats: {psutil.cpu_count()} CPUs, GPU {self.gpu.name if self.gpu else 'n/a'}")

    def cpu_temp(self):
        try:
            temps = psutil.sensors_temperatures()
        except AttributeError:            # Windows
            return NA8
        for key in ("coretemp", "k10temp", "zenpower", "cpu_thermal", "acpitz"):
            for entry in temps.get(key, []):
                if entry.current:
                    return min(254, int(entry.current))
        return NA8

    def sample(self) -> bytes:
        now = time.time()
        dt = max(now - self.t, 1e-3)
        self.t = now
        cpu = min(100, int(psutil.cpu_percent(None)))
        ram = min(100, int(psutil.virtual_memory().percent))
        net = psutil.net_io_counters()
        rx = (net.bytes_recv - self.net.bytes_recv) / dt / 1024
        tx = (net.bytes_sent - self.net.bytes_sent) / dt / 1024
        self.net = net
        rd = wr = NA16
        disk = psutil.disk_io_counters()
        if disk and self.disk:
            rd = (disk.read_bytes - self.disk.read_bytes) / dt / 102400     # units of 100 KB/s
            wr = (disk.write_bytes - self.disk.write_bytes) / dt / 102400
        self.disk = disk
        gpu = vram = gtemp = NA8
        if self.gpu:
            try:
                gpu, vram, gtemp = self.gpu.read()
            except Exception:
                pass

        def u16(v):
            return NA16 if v == NA16 else max(0, min(65534, int(v)))

        return struct.pack("<2sBBBBBHHHHBB", b"SY", 1, cpu, ram, gpu, vram, u16(rx), u16(tx), u16(rd), u16(wr),
                           self.cpu_temp(), gtemp)
