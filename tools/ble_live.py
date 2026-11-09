"""The PC side of the UHK BLE adapter's screen: the live audio spectrum, the
system load for its dashboard and the next meetings for its reminder, over
one Bluetooth link.

    python tools/ble_live.py                 # spectrum of what the PC plays + system stats
    python tools/ble_live.py --agc           # auto-gain for quiet music
    python tools/ble_live.py --calendar URL  # + meeting reminders from an ICS calendar
    python tools/ble_live.py --no-audio      # stats / calendar only
    python tools/ble_live.py --test          # sweep, no audio needed
    python tools/ble_live.py --list          # speakers to pick with --device

Spectrum: captures the audio (WASAPI loopback on Windows, the output's monitor
on Linux), runs the FFT and sends only band levels (1 byte each, ~50 B per
frame); the adapter draws the bars and their ballistics itself (src/live.h).
System load: tools/sysstats.py, 17 B twice a second (src/sysmon.h). Calendar:
tools/calendar_ics.py, a command once a minute (src/cal.h). Defaults for
--no-audio / --stats / --calendar come from the settings file (config_path(),
written by the tray app). Ctrl-C ends it; touching the adapter's screen shows
the normal screen, the spectrum returns after 30 s.
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

    def __init__(self, samplerate, nfft, bands, fmin, fmax, floor, ceil, tilt, gate, agc, fps=60.0):
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
        self.fps = max(fps, 1.0)

    def levels(self, mono) -> bytes:
        mag = np.abs(np.fft.rfft(mono * self.window))
        raw = 20 * np.log10(np.array([mag[g].max() for g in self.groups]) + 1e-12) - self.ref_db
        gain = 0.0
        if self.agc:
            # slow tracker on the loudest (tilted) band: ~30 dB/s up, ~3 dB/s down
            loud = float((raw + self.tilt_db).max())
            if self.agc_state <= self.floor + 0.01 and loud > self.floor:
                self.agc_state = loud                   # first sound after silence: snap
            else:
                rate = (0.5 if loud > self.agc_state else 0.055) * (60.0 / self.fps)
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
    if sys.platform == "darwin" and not args.device:
        import macos_audio
        macos_audio.capture_macos(args, out, stop, Analyzer, Rolling, config_path().parent)
        return
    import soundcard as sc
    spk = sc.get_speaker(args.device) if args.device else sc.default_speaker()
    hop = int(round(args.samplerate / args.fps))
    an = Analyzer(args.samplerate, args.fft, args.bands, args.fmin, args.fmax,
                  args.floor, args.ceil, args.tilt, args.gate, args.agc, args.fps)
    roll = Rolling(args.fft)
    args.analyzer = an                  # the tray app toggles an.agc while it runs
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


async def audio_loop(args, lk, stop, on_state):
    """Band levels at the frame rate; empty frames in silence, because the adapter
    ends the stream after 1.5 s without packets."""
    latest = Latest(asyncio.get_running_loop())
    threading.Thread(target=sweep if args.test else capture, args=(args, latest, stop), daemon=True).start()
    head = b"LV" + bytes([0x01, args.brightness])
    sent, t0, fails = 0, time.time(), 0
    while True:
        try:
            frame = await asyncio.wait_for(latest.get(), 0.5)
        except asyncio.TimeoutError:
            frame = bytes(args.bands)
        if await lk.send_live(head + frame):
            fails = 0
        else:
            fails += 1
            if fails == 1:
                print("write failed", file=sys.stderr)
            if fails > 120:
                raise ConnectionError("the adapter stopped taking packets")
        sent += 1
        if time.time() - t0 >= 5:
            fps = sent / (time.time() - t0)
            if args.verbose:
                print(f"{fps:.1f} frames/s sent")
            if on_state:
                on_state("fps", fps)
            sent, t0 = 0, time.time()


async def keep_awake_loop(lk):
    """macOS rejects min_int=6 (7.5 ms) from src/ble_hid.cpp and applies BLE HID
    peripheral latency = 23 (24 * 15 ms = 360 ms radio sleep when no notifications
    come from the keyboard). Prompting an info notification keeps the adapter's
    radio awake on every 15 ms connection event while the spectrum streams."""
    while True:
        while not lk.events.empty():
            lk.events.get_nowait()
        if not await lk.write_ctrl(b"\x13"):
            raise ConnectionError("the adapter stopped answering on control")


async def stats_loop(args, lk):
    """The system load for the dashboard, twice a second (sysstats.py)."""
    import sysstats
    loop = asyncio.get_running_loop()
    sampler = await loop.run_in_executor(None, sysstats.Sampler)
    fails = 0
    while True:
        pkt = await loop.run_in_executor(None, sampler.sample)
        if await lk.send_live(pkt):
            fails = 0
        else:
            fails += 1
            if fails > 20:
                raise ConnectionError("the adapter stopped taking packets")
        await asyncio.sleep(args.stats_interval)


async def calendar_loop(args, lk):
    """The next meetings (calendar_ics.py): fetched every 5 min, sent whenever the
    packet changes (once a minute, as the minutes count down). A reminder
    dismissed on the screen comes back as a notification."""
    import calendar_ics
    from ble_link import ST_DISMISSED
    cal = calendar_ics.Calendar(args.calendar, config_path().parent)

    def on_notify(data):
        if len(data) >= 3 and data[0] == ST_DISMISSED:
            cal.dismiss(data[1] | data[2] << 8)
    lk.listeners.append(on_notify)
    loop = asyncio.get_running_loop()
    next_fetch, last_packet = 0, None
    while True:
        now = time.time()
        if now >= next_fetch:
            try:
                await loop.run_in_executor(None, cal.refresh)
                next_fetch = now + calendar_ics.REFRESH_S
                print(cal.describe())
            except Exception as e:  # network, a bad URL, a broken feed: try again soon
                print(f"calendar: {e}", file=sys.stderr)
                next_fetch = now + 60
        pkt = cal.packet()
        if pkt != last_packet:
            await lk.command(pkt)
            last_packet = pkt
        await asyncio.sleep(5)


async def run(args, on_state=None):
    """Streams until cancelled. on_state(kind, value): ("connected", device name),
    ("fps", frames per second every 5 s). Raises ConnectionError when the adapter
    stops taking packets (another slot selected, out of range)."""
    from ble_link import Link
    link = Link()
    if getattr(args, "address", None):
        link.address = args.address
    lk = await link.connect(args.name)
    if on_state:
        on_state("connected", lk.name)
    stop = threading.Event()
    tasks = []
    if args.audio:
        tasks.append(asyncio.create_task(audio_loop(args, lk, stop, on_state)))
        if sys.platform == "darwin":
            tasks.append(asyncio.create_task(keep_awake_loop(lk)))
    if args.stats:
        tasks.append(asyncio.create_task(stats_loop(args, lk)))
    if args.calendar and args.calendar_on:
        tasks.append(asyncio.create_task(calendar_loop(args, lk)))
    if not tasks:
        sys.exit("nothing to send: the spectrum, the system stats and the calendar are all off")
    print("sending: " + ", ".join(n for n, on in (("spectrum", args.audio), ("system stats", args.stats),
                                                 ("calendar", args.calendar and args.calendar_on)) if on))
    try:
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_EXCEPTION)
        for t in done:
            t.result()                      # raises what ended it
    finally:
        stop.set()
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if args.audio:
            try:
                await lk.send_live(b"LV\x00")   # stream over: the normal screen comes back
            except OSError:
                pass                            # the link is already gone
        lk.close()                          # an open service blocks the next start for ~20 s


def on_console_close(loop, task):
    """Closing the console window kills the process ~5 s later: cancel the stream
    meanwhile, so the adapter gets its stop packet and the service is released."""
    import ctypes
    done = threading.Event()

    @ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_uint)
    def handler(event):
        if event in (2, 5, 6):          # CTRL_CLOSE / LOGOFF / SHUTDOWN
            loop.call_soon_threadsafe(task.cancel)
            done.wait(3)
            return 1
        return 0                        # Ctrl-C / Ctrl-Break: Python's own handling

    ctypes.windll.kernel32.SetConsoleCtrlHandler(handler, True)
    return handler, done                # the caller keeps the callback alive


async def amain(args):
    keep = on_console_close(asyncio.get_running_loop(), asyncio.current_task()) if os.name == "nt" else None
    try:
        await run(args)
    except asyncio.CancelledError:
        pass
    finally:
        if keep:
            keep[1].set()


def config_path():
    """Settings shared with the tray app (and dismissed.json next to it)."""
    import os
    from pathlib import Path
    if os.name == "nt":
        return Path(os.environ.get("LOCALAPPDATA", Path.home())) / "uhk-ble-live" / "config.json"
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "uhk-ble-live" / "config.json"


CONFIG_DEFAULTS = {"audio": True, "stats": True, "calendar": "", "calendar_on": True, "agc": True}


def load_config() -> dict:
    import json
    try:
        return {**CONFIG_DEFAULTS, **json.loads(config_path().read_text(encoding="utf-8"))}
    except (OSError, ValueError):
        return dict(CONFIG_DEFAULTS)


def save_config(**changes):
    import json
    cfg = {**load_config(), **changes}
    path = config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    return path


def apply_config(args):
    """The settings file into parsed args (the tray app, before every (re)start)."""
    cfg = load_config()
    for key in ("audio", "stats", "calendar", "calendar_on", "agc"):
        setattr(args, key, cfg[key])
    return args


def parse_args(argv=None):
    cfg = load_config()
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", default="UHK60 BLE", help="device name prefix")
    ap.add_argument("--address", help="the adapter's Bluetooth address / identifier (Linux, macOS)")
    ap.add_argument("--no-audio", dest="audio", action="store_false", default=cfg["audio"],
                    help="no spectrum (system stats / calendar only)")
    ap.add_argument("--stats", action=argparse.BooleanOptionalAction, default=cfg["stats"],
                    help="the system load for the adapter's dashboard")
    ap.add_argument("--stats-interval", type=float, default=0.5, help="seconds between load samples")
    ap.add_argument("--calendar", default=cfg["calendar"], metavar="URL",
                    help="ICS calendar (Google: secret address in iCal format) for meeting reminders")
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
    ap.add_argument("--verbose", action="store_true", help="print the frame rate")
    args = ap.parse_args(argv)
    args.bands = max(8, min(64, args.bands))
    args.brightness = max(0, min(255, args.brightness))
    args.calendar_on = cfg["calendar_on"]      # the tray's switch; the URL stays in the settings
    return args


def main(argv=None):
    args = parse_args(argv)
    if args.list:
        import soundcard as sc
        for s in sc.all_speakers():
            print(f"  {s.name}")
        return
    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
