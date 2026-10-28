"""The computer's load for the adapter's dashboard (src/sysmon.h): one "SY"
packet per sample - CPU / RAM / GPU / VRAM in percent, network and disk rates,
CPU / GPU temperatures. psutil for the system, NVML (pynvml) for an NVIDIA GPU
on Windows / Linux, ioreg for the GPU of an Apple Silicon Mac; whatever is not
available goes out as 0xFF.
"""
import struct
import subprocess
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
    """Apple GPU: IOAccelerator's PerformanceStatistics through ioreg (no root)."""
    name = "Apple GPU"

    def __init__(self):
        if sys.platform != "darwin":
            raise OSError("not a Mac")
        self.read()

    def read(self):
        import plistlib
        out = subprocess.run(["ioreg", "-a", "-r", "-d", "1", "-c", "IOAccelerator"],
                             capture_output=True, timeout=5).stdout
        for node in plistlib.loads(out):
            stats = node.get("PerformanceStatistics", {})
            if "Device Utilization %" in stats:
                return int(stats["Device Utilization %"]), NA8, NA8
        return NA8, NA8, NA8


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
