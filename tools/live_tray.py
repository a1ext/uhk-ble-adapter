"""Tray app of the adapter's PC side (dist/uhk-ble-live.exe, tools/build_exe.py):
the live spectrum, the system stats and the calendar reminders of
tools/ble_live.py, no console window, an icon in the notification area.

Menu: status, Pause / Resume (also a left click), the three feature switches,
Auto gain, Edit settings (config.json: the calendar URL goes there), Open log,
Exit. Switches are saved and restart the stream; an edited settings file is
read at the next (re)start (Pause / Resume). When the adapter is gone (another
slot selected, out of range, off) the icon turns grey and it reconnects every
10 s. Arguments are those of tools/ble_live.py. Files in
%LOCALAPPDATA%/uhk-ble-live: live.log, config.json, dismissed.json.
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
            ble_live.apply_config(self.args)     # edited settings take effect at the next (re)start
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
    args = ble_live.parse_args(argv)         # the feature switches come from the settings file
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
        ble_live.save_config(agc=args.agc)
        if getattr(args, "analyzer", None):
            args.analyzer.agc = args.agc

    def quit_app(_icon, _item):
        streamer.stop()
        icon.stop()

    def open_log(_icon, _item):
        if log:
            os.startfile(log)

    def restart():
        if not streamer.paused:
            streamer.stop()
            streamer.start()

    def toggle(key):                         # a feature switch: saved, the stream restarts
        def handler(_icon, _item):
            setattr(args, key, not getattr(args, key))
            ble_live.save_config(**{key: getattr(args, key)})
            restart()
            refresh()
        return handler

    def open_settings(_icon, _item):
        os.startfile(ble_live.save_config())   # written with the defaults when there is none yet

    def calendar_item(_icon, _item):
        if args.calendar:
            toggle("calendar_on")(_icon, _item)
        else:                                # no URL yet: the settings file is where it goes
            open_settings(_icon, _item)

    icon.menu = pystray.Menu(
        pystray.MenuItem(lambda _: "Paused" if streamer.paused else streamer.status, None, enabled=False),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem(lambda _: "Resume" if streamer.paused else "Pause", toggle_pause, default=True),
        pystray.MenuItem("Audio spectrum", toggle("audio"), checked=lambda _: args.audio),
        pystray.MenuItem("Auto gain", toggle_agc, checked=lambda _: args.agc),
        pystray.MenuItem("System stats", toggle("stats"), checked=lambda _: args.stats),
        pystray.MenuItem(lambda _: "Calendar reminders" if args.calendar else "Calendar reminders (set the URL)",
                         calendar_item, checked=lambda _: bool(args.calendar) and args.calendar_on),
        pystray.MenuItem("Edit settings", open_settings),
        pystray.MenuItem("Open log", open_log, visible=log is not None),
        pystray.MenuItem("Exit", quit_app),
    )

    def setup(ic):
        ic.visible = True
        streamer.start()
    icon.run(setup)


if __name__ == "__main__":
    main()
