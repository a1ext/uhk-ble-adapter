"""Live audio spectrum on the UHK BLE adapter's screen (Windows).

    python tools/ble_live.py                 # what the PC plays (WASAPI loopback)
    python tools/ble_live.py --agc           # auto-gain for quiet music
    python tools/ble_live.py --test          # sweep, no audio needed
    python tools/ble_live.py --list          # speakers to pick with --device

Captures the audio, runs the FFT and sends only band levels (1 byte each,
~50 B per frame); the adapter draws the bars and their ballistics itself.
Protocol: src/live.h. Needs the WinRT projections (`pip install bleak`) and
`soundcard` for the loopback capture. Ctrl-C ends it; touching the adapter's
screen shows the normal screen, the spectrum returns after 30 s.
"""
import argparse
import asyncio
import math
import os
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

AGC_HEADROOM = 5.0  # dB kept free above the tracked level, so peaks still reach the top


class Rolling:
    """The last nfft samples: the FFT window overlaps between frames, so the
    frame rate does not depend on the window length."""

    def __init__(self, nfft):
        self.buf = np.zeros(nfft, dtype=np.float32)

    def push(self, block):
        k = block.shape[0]
        if k >= self.buf.size:
            self.buf[:] = block[-self.buf.size:]
        elif k:
            self.buf[:-k] = self.buf[k:]
            self.buf[-k:] = block
        return self.buf


class Analyzer:
    """Windowed rfft -> log-spaced bands -> levels 0..255."""

    def __init__(self, samplerate, nfft, bands, fmin, fmax, floor, ceil, tilt, gate, agc):
        edges = np.geomspace(fmin, min(fmax, samplerate / 2 * 0.99), bands + 1)
        freqs = np.fft.rfftfreq(nfft, 1.0 / samplerate)
        self.groups = []
        for lo, hi in zip(edges[:-1], edges[1:]):
            sel = np.where((freqs >= lo) & (freqs < hi))[0]
            if sel.size == 0:  # band narrower than one bin: the nearest one
                sel = np.array([int(np.argmin(np.abs(freqs - (lo + hi) / 2)))])
            self.groups.append(sel)
        centers = np.sqrt(edges[:-1] * edges[1:])
        self.window = np.hanning(nfft).astype(np.float32)
        self.ref_db = 20 * math.log10(nfft / 4.0)        # full-scale sine through Hann
        self.tilt_db = tilt * np.log2(centers / centers[0])  # music falls ~3 dB/octave
        self.floor, self.ceil, self.span = floor, ceil, max(ceil - floor, 1.0)
        self.gate, self.agc, self.agc_state = gate, agc, ceil

    def levels(self, mono) -> bytes:
        mag = np.abs(np.fft.rfft(mono * self.window))
        raw = 20 * np.log10(np.array([mag[g].max() for g in self.groups]) + 1e-12) - self.ref_db
        gain = 0.0
        if self.agc:
            # slow tracker on the loudest (tilted) band: ~30 dB/s up, ~3 dB/s down at 60 fps
            loud = float((raw + self.tilt_db).max())
            if self.agc_state <= self.floor + 0.01 and loud > self.floor:
                self.agc_state = loud                   # first sound after silence: snap
            else:
                rate = 0.5 if loud > self.agc_state else 0.055
                self.agc_state += float(np.clip(loud - self.agc_state, -rate, rate))
            self.agc_state = max(self.agc_state, self.floor)
            gain = float(np.clip(self.ceil - AGC_HEADROOM - self.agc_state, -20.0, 30.0))
        lv = np.clip((raw + gain + self.tilt_db - self.floor) / self.span, 0.0, 1.0)
        lv[raw < self.gate] = 0.0                       # gate before the tilt: no hiss in the treble
        return (lv * 255).astype(np.uint8).tobytes()


class Latest:
    """Hands the newest frame from the capture thread to the BLE loop; older
    unsent frames are dropped, so a slow link never builds up a delay."""

    def __init__(self, loop):
        self.loop, self.event, self.frame = loop, asyncio.Event(), None

    def put(self, frame):
        def set_():
            self.frame = frame
            self.event.set()
        self.loop.call_soon_threadsafe(set_)

    async def get(self):
        await self.event.wait()
        self.event.clear()
        return self.frame


def capture(args, out: Latest, stop: threading.Event):
    import soundcard as sc
    spk = sc.get_speaker(args.device) if args.device else sc.default_speaker()
    hop = int(round(args.samplerate / args.fps))
    an = Analyzer(args.samplerate, args.fft, args.bands, args.fmin, args.fmax,
                  args.floor, args.ceil, args.tilt, args.gate, args.agc)
    roll = Rolling(args.fft)
    print(f"loopback: {spk.name} @ {args.samplerate} Hz | window {args.fft} "
          f"({1000 * args.fft / args.samplerate:.0f} ms) | {args.bands} bands @ {args.fps:.0f} fps")
    while not stop.is_set():
        try:
            mic = sc.get_microphone(spk.id, include_loopback=True)
            with mic.recorder(samplerate=args.samplerate, channels=2, blocksize=hop) as rec:
                while not stop.is_set():
                    block = rec.record(numframes=hop)
                    mono = block.mean(axis=1) if block.ndim > 1 else block
                    if not mono.size:   # WASAPI hands back nothing while the endpoint idles
                        time.sleep(1 / args.fps)
                    out.put(an.levels(roll.push(mono.astype(np.float32))))
        except Exception as exc:  # endpoint changed / vanished: reopen
            print(f"capture dropped ({exc}); reopening", file=sys.stderr)
            time.sleep(0.5)


def sweep(args, out: Latest, stop: threading.Event):
    print("test sweep, Ctrl-C to stop")
    pos = 0.0
    while not stop.is_set():
        lv = [max(0.0, 1.0 - abs(i - pos) / 5.0) for i in range(args.bands)]
        out.put(bytes(int(v * 255) for v in lv))
        pos = (pos + 0.35) % args.bands
        time.sleep(1 / args.fps)


async def run(args):
    from ble_link import Link
    lk = await Link().connect(args.name)
    latest = Latest(asyncio.get_running_loop())
    stop = threading.Event()
    threading.Thread(target=sweep if args.test else capture, args=(args, latest, stop), daemon=True).start()
    head = b"LV" + bytes([0x01, args.brightness])
    sent, t0 = 0, time.time()
    try:
        while True:
            # the adapter ends the stream after 1.5 s without packets: keep sending in silence
            try:
                frame = await asyncio.wait_for(latest.get(), 0.5)
            except asyncio.TimeoutError:
                frame = bytes(args.bands)
            if not await lk.send_live(head + frame):
                print("write failed", file=sys.stderr)
            sent += 1
            if args.stats and time.time() - t0 >= 5:
                print(f"{sent / (time.time() - t0):.1f} frames/s sent")
                sent, t0 = 0, time.time()
    finally:
        stop.set()
        await lk.send_live(b"LV\x00")   # stream over: the normal screen comes back


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", default="UHK60 BLE", help="device name prefix")
    ap.add_argument("--list", action="store_true", help="list speakers and exit")
    ap.add_argument("--device", help="speaker name (substring) to capture, default = Windows default")
    ap.add_argument("--test", action="store_true", help="sweep pattern, no audio")
    ap.add_argument("--bands", type=int, default=48, help="8..64")
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--fft", type=int, default=2048, help="window in samples: bigger = finer bass, more smear")
    ap.add_argument("--samplerate", type=int, default=48000)
    ap.add_argument("--agc", action="store_true", help="slow auto-gain so quiet passages still fill the bars")
    ap.add_argument("--fmin", type=float, default=40.0)
    ap.add_argument("--fmax", type=float, default=16000.0)
    ap.add_argument("--floor", type=float, default=-62.0, help="dBFS for an empty bar")
    ap.add_argument("--ceil", type=float, default=-6.0, help="dBFS for a full bar")
    ap.add_argument("--tilt", type=float, default=3.0, help="dB per octave lift for the treble")
    ap.add_argument("--gate", type=float, default=-72.0, help="dBFS noise gate (before the tilt)")
    ap.add_argument("--brightness", type=int, default=0,
                    help="1-255 fixed, 0 = by the time of day (ble_anim.py set --brightness / --night)")
    ap.add_argument("--stats", action="store_true", help="print the frame rate")
    args = ap.parse_args()
    args.bands = max(8, min(64, args.bands))
    args.brightness = max(0, min(255, args.brightness))

    if args.list:
        import soundcard as sc
        for s in sc.all_speakers():
            print(f"  {s.name}")
        return
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
