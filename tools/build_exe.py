"""Builds the Windows tools as single executables that need no Python on the
target PC:

- dist/uhk-ble-live.exe: the live spectrum as a tray app (tools/live_tray.py
  around tools/ble_live.py), no console window;
- dist/uhk-ble-anim.exe: tools/ble_anim.py (console; a GIF / folder dropped on
  it is uploaded).

    python tools/build_exe.py [live] [anim]     # both by default

Creates its own virtual environment .venv-build (with uv when it is on PATH,
else python -m venv) with PyInstaller, bleak (WinRT), soundcard, numpy, pystray
and Pillow; the Python that runs this script only needs Pillow (for the icon).
CI (.github/workflows/build.yml) runs it and publishes both on the web page.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VENV = ROOT / ".venv-build"
WORK = ROOT / ".pio" / "pyinstaller"
PACKAGES = ["pyinstaller", "bleak", "soundcard", "numpy", "pystray", "pillow"]

sys.path.insert(0, str(ROOT / "tools"))
import live_icon  # noqa: E402

TARGETS = {
    "live": ["--windowed", "--name", "uhk-ble-live",
             "--hidden-import", "pystray._win32",     # pystray picks its backend at run time
             "--collect-data", "soundcard",           # its cffi headers (mediafoundation.py.h)
             ROOT / "tools" / "live_tray.py"],
    "anim": ["--console", "--name", "uhk-ble-anim",
             "--hidden-import", "anim_demos",         # imported for demo:<name> only
             ROOT / "tools" / "ble_anim.py"],
}


def run(*cmd):
    print(">", " ".join(map(str, cmd)))
    subprocess.run(list(map(str, cmd)), check=True, cwd=ROOT)


def main():
    names = sys.argv[1:] or list(TARGETS)
    if any(n not in TARGETS for n in names):
        sys.exit(f"targets: {', '.join(TARGETS)}")
    py = VENV / "Scripts" / "python.exe"
    if not py.exists():
        if shutil.which("uv"):
            run("uv", "venv", "--python", f"{sys.version_info.major}.{sys.version_info.minor}", VENV)
        else:
            run(sys.executable, "-m", "venv", VENV)
    if shutil.which("uv"):
        run("uv", "pip", "install", "--python", py, *PACKAGES)
    else:
        run(py, "-m", "pip", "install", *PACKAGES)

    WORK.mkdir(parents=True, exist_ok=True)
    icon = WORK / "uhk-ble-live.ico"
    live_icon.save_ico(icon)
    for name in names:
        run(py, "-m", "PyInstaller", "--noconfirm", "--onefile", "--icon", icon,
            "--paths", ROOT / "tools",
            "--collect-submodules", "winrt",         # WinRT projections are imported by dotted name
            "--exclude-module", "tkinter",
            "--distpath", ROOT / "dist", "--workpath", WORK, "--specpath", WORK,
            *TARGETS[name])
    for name in names:
        exe = ROOT / "dist" / f"uhk-ble-{name}.exe"
        print(f"{exe} ({exe.stat().st_size / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
