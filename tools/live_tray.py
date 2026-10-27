"""Tray app of the live spectrum (dist/uhk-ble-live.exe, tools/build_live_exe.py):
no console window, an icon in the notification area.

Menu: status, Pause / Resume (also a left click), Auto gain, Exit. When the
adapter is gone (another slot selected, out of range, off) the icon turns grey
and it reconnects every 10 s. Arguments are those of tools/ble_live.py (auto
gain on by default). Log: %LOCALAPPDATA%/uhk-ble-live/live.log.
"""
import asyncio
import ctypes
import os
import sys
import threading
import time
from pathlib import Path

RETRY_S = 10
APP = "UHK BLE live spectrum"


def setup_log():
    """No console: print() and warnings go to a log file (sys.stdout is None here)."""
    folder = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "uhk-ble-live"
    folder.mkdir(parents=True, exist_ok=True)
    log = open(folder / "live.log", "w", encoding="utf-8", buffering=1)
    sys.stdout = sys.stderr = log
    print(time.strftime("%Y-%m-%d %H:%M:%S"), APP)
    return folder / "live.log"


def single_instance():
    ctypes.windll.kernel32.CreateMutexW(None, False, "Local\\uhk-ble-live")
    if ctypes.windll.kernel32.GetLastError() == 183:   # ERROR_ALREADY_EXISTS
        ctypes.windll.user32.MessageBoxW(None, "The live spectrum is already running (tray icon).", APP, 0x40)
        sys.exit(0)


class Streamer:
    """ble_live.run() in a thread of its own, restarted after errors."""

    def __init__(self, args, on_change):
        self.args, self.on_change = args, on_change
        self.status, self.active, self.paused = "Starting...", False, False
        self.loop = asyncio.new_event_loop()
        self.task = None
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.last_error = None

    def set(self, status, active):
        self.status, self.active = status, active
        print(status)
        self.on_change()

    def start(self):
        self.paused = False
        self.loop.call_soon_threadsafe(self._spawn)

    def _spawn(self):
        if not self.task or self.task.done():
            self.task = self.loop.create_task(self._forever())

    async def _forever(self):
        import ble_live
        device = None

        def on_state(kind, value):
            nonlocal device
            if kind == "connected":
                device = value
                self.last_error = None
                self.set(f"{device}: streaming", True)
            elif kind == "fps":
                self.set(f"{device}: {value:.0f} frames/s", True)

        while True:
            self.set("Connecting to the adapter...", False)
            try:
                await ble_live.run(self.args, on_state)
            except asyncio.CancelledError:
                raise
            except SystemExit as e:              # ble_link: no paired adapter / no service
                self.error(str(e.code))
            except Exception as e:
                self.error(f"{type(e).__name__}: {e}")
            await asyncio.sleep(RETRY_S)

    def error(self, text):
        self.set(f"Not connected - retrying every {RETRY_S} s", False)
        print(text)
        if text != self.last_error:              # notify once per distinct problem
            self.last_error = text
            self.notify(text)

    def notify(self, text):
        pass                                     # set by the tray

    def stop(self, wait=4.0):
        """Cancel the stream: the adapter gets its stop packet, the service is released."""
        done = threading.Event()

        def cancel():
            if self.task and not self.task.done():
                self.task.add_done_callback(lambda _: done.set())
                self.task.cancel()
            else:
                done.set()
        self.loop.call_soon_threadsafe(cancel)
        done.wait(wait)


def main():
    if getattr(sys, "frozen", False) or sys.stdout is None:
        log = setup_log()
    else:
        log = None
    single_instance()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import ble_live
    import live_icon
    import pystray

    argv = sys.argv[1:]
    args = ble_live.parse_args(argv if "--agc" in argv else argv + ["--agc"])   # off in the menu
    icons = {True: live_icon.make(True, 64), False: live_icon.make(False, 64)}
    icon = pystray.Icon("uhk-ble-live", icons[False], APP)

    def refresh():
        icon.icon = icons[streamer.active and not streamer.paused]
        icon.title = f"{APP}\n{'Paused' if streamer.paused else streamer.status}"[:127]
        icon.update_menu()

    streamer = Streamer(args, refresh)
    streamer.notify = lambda text: icon.notify(text[:250], APP)

    def toggle_pause(_icon=None, _item=None):
        if streamer.paused:
            streamer.start()
        else:
            streamer.paused = True
            streamer.stop()
        refresh()

    def toggle_agc(_icon, _item):
        args.agc = not args.agc
        if getattr(args, "analyzer", None):
            args.analyzer.agc = args.agc

    def quit_app(_icon, _item):
        streamer.stop()
        icon.stop()

    def open_log(_icon, _item):
        if log:
            os.startfile(log)

    icon.menu = pystray.Menu(
        pystray.MenuItem(lambda _: "Paused" if streamer.paused else streamer.status, None, enabled=False),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem(lambda _: "Resume" if streamer.paused else "Pause", toggle_pause, default=True),
        pystray.MenuItem("Auto gain", toggle_agc, checked=lambda _: args.agc),
        pystray.MenuItem("Open log", open_log, visible=log is not None),
        pystray.MenuItem("Exit", quit_app),
    )

    def setup(ic):
        ic.visible = True
        streamer.start()
    icon.run(setup)


if __name__ == "__main__":
    main()
