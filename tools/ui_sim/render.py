"""Renders the adapter's screen (src/ui.cpp with LVGL, the live spectrum of
src/live.cpp, the matrix standby animation) on a PC into the README / web page
screenshots docs/img/screen-*.png.

    python tools/ui_sim/render.py

Needs:
- gcc / g++ on PATH (Windows: e.g. TDM-GCC or MSYS2 MinGW; override with CC / CXX);
- LVGL from PlatformIO's libdeps: run `pio run` once (or pass --lvgl);
- Python 3 with Pillow and numpy (`pip install pillow numpy`).

The scenes and their canned status data are in sim.cpp. LVGL objects are cached
in .pio/ui_sim, so only the first run compiles LVGL. Besides the screenshots it
leaves anim-*.png (frames of a slot switch) in .pio/ui_sim for a quick look.
"""
import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
W, H = 536, 240
SCREENSHOTS = ["screen-bt", "screen-passkey", "screen-agent", "screen-menu", "screen-rename", "screen-live"]
FRAMES = ["anim-spark", "anim-pop"]


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"failed: {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")


def build_stamp():
    """__DATE__ / __TIME__ for ui.cpp: the time of the last commit that touched
    src/, so the same firmware always renders the same screenshots (the screen
    shows the build time), also after the screenshots themselves are committed."""
    r = subprocess.run(["git", "log", "-1", "--format=%cd", "--date=format:%b %e %Y|%H:%M:%S", "--", "src"],
                       cwd=ROOT, capture_output=True, text=True)
    date, _, time = r.stdout.strip().partition("|")
    return (date, time) if time else ("Jan  1 2026", "00:00:00")


def compile_lvgl(cc, flags, lvgl, out):
    conf = [HERE / "inc" / "lv_conf.h", ROOT / "lib" / "LilyGo-AMOLED-Series" / "src" / "lv_conf.h"]
    newest_conf = max(p.stat().st_mtime for p in conf)
    jobs = []
    for c in sorted((lvgl / "src").rglob("*.c")):
        o = out / c.relative_to(lvgl).with_suffix(".o")
        if not o.exists() or o.stat().st_mtime < max(c.stat().st_mtime, newest_conf):
            o.parent.mkdir(parents=True, exist_ok=True)
            jobs.append([cc, *flags, "-c", str(c), "-o", str(o)])
    if jobs:
        print(f"compiling LVGL ({len(jobs)} files)...")
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            list(pool.map(run, jobs))
    return sorted(out.rglob("*.o"))


def frame(img, scale=2, pad=14, radius=26):
    """Dark rounded bezel; pixels scaled up without blur (sharp on HiDPI screens)."""
    big = img.resize((W * scale, H * scale), Image.NEAREST)
    w, h = (W + 2 * pad) * scale, (H + 2 * pad) * scale
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(out).rounded_rectangle([0, 0, w - 1, h - 1], radius=radius * scale,
                                          fill=(18, 18, 20, 255), outline=(44, 44, 48, 255), width=2)
    out.paste(big, (pad * scale, pad * scale))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lvgl", type=Path, default=ROOT / ".pio" / "libdeps" / "t-display-s3-amoled-plus" / "lvgl")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "img")
    args = ap.parse_args()
    if not (args.lvgl / "lvgl.h").exists():
        sys.exit(f"LVGL not found in {args.lvgl}: run `pio run` once, or pass --lvgl")

    cc, cxx = os.environ.get("CC", "gcc"), os.environ.get("CXX", "g++")
    build = ROOT / ".pio" / "ui_sim"
    build.mkdir(parents=True, exist_ok=True)
    flags = ["-O1", "-DLV_CONF_INCLUDE_SIMPLE", "-I", str(HERE / "inc"), "-I", str(args.lvgl), "-I", str(ROOT / "src")]

    objs = compile_lvgl(cc, flags, args.lvgl, build / "lvgl")
    date, time = build_stamp()
    stamp = ["-Wno-builtin-macro-redefined", f'-D__DATE__="{date}"', f'-D__TIME__="{time}"']
    run([cxx, "-std=gnu++17", *flags, *stamp, "-c", str(ROOT / "src" / "ui.cpp"), "-o", str(build / "ui.o")])
    run([cxx, "-std=gnu++17", *flags, "-c", str(ROOT / "src" / "live.cpp"), "-o", str(build / "live.o")])
    run([cxx, "-std=gnu++17", *flags, "-c", str(HERE / "sim.cpp"), "-o", str(build / "sim.o")])
    exe = build / ("uisim.exe" if os.name == "nt" else "uisim")
    run([cxx, "-o", str(exe), str(build / "sim.o"), str(build / "ui.o"), str(build / "live.o"), *map(str, objs)])
    run([str(exe)], cwd=build)

    args.out.mkdir(parents=True, exist_ok=True)
    for name in SCREENSHOTS:
        img = Image.frombytes("RGB", (W, H), (build / f"{name}.rgb").read_bytes())
        frame(img).save(args.out / f"{name}.png", optimize=True)
        print(args.out / f"{name}.png")
    for name in FRAMES:
        Image.frombytes("RGB", (W, H), (build / f"{name}.rgb").read_bytes()).save(build / f"{name}.png")

    frame(matrix_frame()).save(args.out / "screen-matrix.png", optimize=True)
    print(args.out / "screen-matrix.png")


def matrix_frame(index=40):
    """A frame of the matrix demo, encoded and decoded the way the adapter plays it."""
    sys.path.insert(0, str(ROOT / "tools"))
    import anim_demos
    import numpy as np
    import uanm
    scale = 2
    blob = uanm.encode([uanm.fit(f, scale) for f in anim_demos.matrix_rain()], 40, scale)
    v = uanm.decode(blob)[index]
    v = (((v >> 8) | (v << 8)) & 0xFFFF).astype(np.uint32)       # back to plain RGB565
    rgb = np.dstack([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31])
    pic = Image.fromarray(rgb.astype(np.uint8))
    pic = pic.resize((pic.width * scale, pic.height * scale), Image.NEAREST)
    out = Image.new("RGB", (W, H))
    out.paste(pic, (((W - pic.width) // 2) & ~1, ((H - pic.height) // 2) & ~1))
    return out


if __name__ == "__main__":
    main()
