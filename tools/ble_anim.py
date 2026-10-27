"""Standby animations for the UHK BLE adapter: build, upload over Bluetooth, control.

    python tools/ble_anim.py make demo:matrix -o matrix.uanm     # or a GIF / WebP / PNG folder
    python tools/ble_anim.py preview matrix.uanm -o check.gif     # decode it back to look at
    python tools/ble_anim.py upload matrix.uanm                   # no reboot, plays from now on
    python tools/ble_anim.py play | stop | info
    python tools/ble_anim.py set --mode standby --delay 60 --brightness 180 --night 30 --off-after 1800

The upload and the commands use Windows' own connection to the paired adapter
(like tools/ble_ota.py) and need the WinRT projections (`pip install bleak`).
File format and commands: src/anim.h.
"""
import argparse
import asyncio
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
import uanm  # noqa: E402
from PIL import Image, ImageSequence  # noqa: E402

MODES = {"off": 0, "standby": 1}


def load_frames(src: str, scale: int, fps, max_frames: int):
    """-> (frames, frame_ms) from demo:<name>, an animated image or a folder of images."""
    if src.startswith("demo:"):
        import anim_demos
        name = src[5:]
        if name not in anim_demos.DEMOS:
            sys.exit(f"unknown demo '{name}', have: {', '.join(anim_demos.DEMOS)}")
        frames, ms = anim_demos.DEMOS[name](), 40
    elif os.path.isdir(src):
        files = sorted(p for p in Path(src).iterdir() if p.suffix.lower() in (".png", ".jpg", ".bmp", ".gif"))
        frames, ms = [Image.open(p).convert("RGB") for p in files], 50
    else:
        im = Image.open(src)
        frames = [f.convert("RGB") for f in ImageSequence.Iterator(im)]
        ms = im.info.get("duration") or 50
    if fps:
        ms = int(round(1000 / fps))
    frames = frames[:max_frames]
    size = uanm.fit(frames[0], scale).size
    frames = [f if f.size == size else f.resize(size, Image.LANCZOS) for f in
              (uanm.fit(f, scale) for f in frames)]
    return frames, max(10, int(ms))


def build(args) -> bytes:
    if args.input.lower().endswith(".uanm"):
        return Path(args.input).read_bytes()
    bg = tuple(int(args.bg.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))
    frames, ms = load_frames(args.input, args.scale, args.fps, args.max_frames)
    blob = uanm.encode(frames, ms, args.scale, bg)
    w, h = frames[0].size
    print(f"{len(frames)} frames {w}x{h} x{args.scale}, {ms} ms/frame "
          f"({len(frames) * ms / 1000:.1f} s loop), {len(blob) / 1024:.0f} KB")
    if len(blob) > 0x360000:
        sys.exit("too big for the adapter's 3.4 MB data partition: fewer frames, scale 2 or a smaller size")
    return blob


def cmd_make(args):
    blob = build(args)
    Path(args.output).write_bytes(blob)
    print(f"written {args.output}")


def cmd_preview(args):
    blob = build(args)
    _, _, scale, w, h, n, ms = struct.unpack_from("<4sBBHHHH", blob)
    frames = []
    for f in uanm.decode(blob):
        v = (((f >> 8) | (f << 8)) & 0xFFFF).astype(np.uint32)   # back to plain RGB565
        rgb = np.dstack([((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31])
        frames.append(Image.fromarray(rgb.astype(np.uint8)).resize((w * scale, h * scale), Image.NEAREST))
    frames[0].save(args.output, save_all=True, append_images=frames[1:], duration=ms, loop=0)
    print(f"preview {args.output}: {n} frames as the adapter shows them")


async def link(args):
    from ble_link import Link
    return await Link().connect(args.name)


async def query(lk):
    """-> (frames, w, h, scale, ms, mode, delay, bright, off, night, clock or None)"""
    await lk.command(bytes([0x13]))
    while True:
        v = await lk.event(5)
        if v[:1] == b"\x10" and len(v) >= 17:
            _, _, frames, w, h, scale, ms, mode, delay, bright, off = struct.unpack("<BBHHHBHBHBH", v[:17])
            night, clock = bright, None              # firmware before the day / night brightness
            if len(v) >= 21:
                night, valid, hh, mm = v[17:21]
                clock = f"{hh:02d}:{mm:02d}" if valid else None
            return frames, w, h, scale, ms, mode, delay, bright, off, night, clock


async def cmd_upload(args):
    from ble_link import BEGIN_ANIMATION
    blob = build(args)
    lk = await link(args)
    await lk.upload(blob, BEGIN_ANIMATION, what="animation")
    print("stored - the adapter plays it in standby (no reboot); 'play' shows it right away")


async def cmd_simple(args):
    lk = await link(args)
    await lk.command(bytes([{"play": 0x10, "stop": 0x11}[args.cmd]]))
    print("sent")


async def cmd_info(args):
    lk = await link(args)
    frames, w, h, scale, ms, mode, delay, bright, off, night, clock = await query(lk)
    if frames:
        print(f"animation: {frames} frames {w}x{h} x{scale}, {ms} ms/frame ({frames * ms / 1000:.1f} s loop)")
    else:
        print("animation: none stored")
    print(f"standby: {'animation' if mode else 'off'} after {delay} s idle, "
          f"screen off after {off} s of animation" + (" (never)" if not off else ""))
    print(f"brightness: day {bright}, night {night} (animation and live spectrum); "
          f"adapter clock {clock or 'not set'}")


async def cmd_set(args):
    lk = await link(args)
    _, _, _, _, _, mode, delay, bright, off, night, _ = await query(lk)
    mode = MODES[args.mode] if args.mode else mode
    delay = args.delay if args.delay is not None else delay
    bright = args.brightness if args.brightness is not None else bright
    night = args.night if args.night is not None else night
    off = args.off_after if args.off_after is not None else off
    await lk.command(struct.pack("<BBHBHB", 0x12, mode, delay, bright, off, night))
    print(f"standby: {'animation' if mode else 'off'} after {delay} s, brightness day {bright} / "
          f"night {night}, off after {off} s")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", default="UHK60 BLE", help="device name prefix")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def source(p):
        p.add_argument("input", help="demo:<name>, .gif / .webp / .png, a folder of images, or .uanm")
        p.add_argument("--scale", type=int, choices=(1, 2), default=2,
                       help="2 = half resolution, pixel-doubled (4x less data)")
        p.add_argument("--fps", type=float, help="override the frame rate")
        p.add_argument("--bg", default="#000000", help="background around the picture")
        p.add_argument("--max-frames", type=int, default=4096)

    p = sub.add_parser("make", help="build a .uanm file")
    source(p)
    p.add_argument("-o", "--output", required=True)
    p = sub.add_parser("preview", help="decode to a GIF the way the adapter shows it")
    source(p)
    p.add_argument("-o", "--output", required=True)
    p = sub.add_parser("upload", help="store on the adapter (no reboot)")
    source(p)
    sub.add_parser("play", help="show the animation now (until stopped or touched)")
    sub.add_parser("stop", help="back to the normal screen")
    sub.add_parser("info", help="stored animation and standby settings")
    p = sub.add_parser("set", help="standby settings (unset options keep their value)")
    p.add_argument("--mode", choices=MODES)
    p.add_argument("--delay", type=int, help="idle seconds before the animation (min 5)")
    p.add_argument("--brightness", type=int, help="1-255 by day (9:00-18:00) while it plays")
    p.add_argument("--night", type=int, help="1-255 at night (22:00-6:00); ramps in between")
    p.add_argument("--off-after", type=int, help="screen off after this many seconds of animation, 0 = never")
    args = ap.parse_args()

    if args.cmd == "make":
        cmd_make(args)
    elif args.cmd == "preview":
        cmd_preview(args)
    else:
        coro = {"upload": cmd_upload, "info": cmd_info, "set": cmd_set}.get(args.cmd, cmd_simple)
        asyncio.run(coro(args))


if __name__ == "__main__":
    main()
