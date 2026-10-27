"""UANM encoder: frames (PIL images) -> the adapter's standby animation file (src/anim.h).

Every frame is stored as a delta to the previous one: skips over unchanged pixels,
runs of one colour, literals. Pixels are RGB565, byte-swapped the way the panel
takes them.
"""
import struct

import numpy as np
from PIL import Image

SCREEN_W, SCREEN_H = 536, 240
MAX_RUN = 0x4000            # pixels per token
MIN_RUN = 3                 # shorter repeats go into literals


def rgb565_swapped(img: Image.Image) -> np.ndarray:
    a = np.asarray(img.convert("RGB"), dtype=np.uint16)
    v = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
    return ((v >> 8) | (v << 8)) & 0xFFFF


def color565_swapped(rgb) -> int:
    r, g, b = rgb
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return ((v >> 8) | (v << 8)) & 0xFFFF


def _tokens(op, n):
    out = []
    while n > 0:
        k = min(n, MAX_RUN)
        out.append(struct.pack("<H", op << 14 | (k - 1)))
        n -= k
    return out


def encode_frame(cur: np.ndarray, prev: np.ndarray) -> bytes:
    """cur, prev: flat uint16 arrays. Trailing unchanged pixels need no token."""
    out = []
    changed = cur != prev
    n = len(cur)
    if not changed.any():
        return b""
    # segments of equal 'changed'
    edges = np.flatnonzero(np.diff(changed.astype(np.int8))) + 1
    starts = np.concatenate(([0], edges))
    ends = np.concatenate((edges, [n]))
    last_changed_end = ends[changed[starts]][-1]
    for s, e in zip(starts, ends):
        if s >= last_changed_end:
            break
        if not changed[s]:
            out += _tokens(0, e - s)
            continue
        seg = cur[s:e]
        # runs of equal values inside the changed segment
        redges = np.flatnonzero(np.diff(seg)) + 1
        rstarts = np.concatenate(([0], redges))
        rends = np.concatenate((redges, [len(seg)]))
        lit_from = None
        for rs, re in zip(rstarts, rends):
            if re - rs >= MIN_RUN:
                if lit_from is not None:
                    lit = seg[lit_from:rs]
                    for i in range(0, len(lit), MAX_RUN):
                        part = lit[i:i + MAX_RUN]
                        out += _tokens(2, len(part))
                        out.append(part.astype("<u2").tobytes())
                    lit_from = None
                left = re - rs
                while left:
                    k = min(left, MAX_RUN)
                    out.append(struct.pack("<HH", 1 << 14 | (k - 1), int(seg[rs])))
                    left -= k
            elif lit_from is None:
                lit_from = rs
        if lit_from is not None:
            lit = seg[lit_from:]
            for i in range(0, len(lit), MAX_RUN):
                part = lit[i:i + MAX_RUN]
                out += _tokens(2, len(part))
                out.append(part.astype("<u2").tobytes())
    return b"".join(out)


def fit(img: Image.Image, scale: int) -> Image.Image:
    """Resize to fit the screen at the given pixel scale; even sizes on screen."""
    box_w, box_h = SCREEN_W // scale, SCREEN_H // scale
    w, h = img.size
    k = min(box_w / w, box_h / h)
    w, h = max(2, int(w * k)), max(2, int(h * k))
    if scale == 1:
        w, h = w & ~1, h & ~1
    img = img.convert("RGB")
    return img.resize((w, h), Image.LANCZOS) if (w, h) != img.size else img


def encode(frames, frame_ms: int, scale: int = 1, background=(0, 0, 0)) -> bytes:
    """frames: PIL images, all the same size (fit() them first)."""
    if not frames:
        raise ValueError("no frames")
    w, h = frames[0].size
    if (w * scale) % 2 or (h * scale) % 2 or w * scale > SCREEN_W or h * scale > SCREEN_H:
        raise ValueError(f"{w}x{h} x{scale} does not fit {SCREEN_W}x{SCREEN_H} with even sizes")
    bg = color565_swapped(background)
    prev = np.full(w * h, bg, dtype=np.uint16)
    blobs = []
    for f in frames:
        if f.size != (w, h):
            raise ValueError("frames differ in size")
        cur = rgb565_swapped(f).reshape(-1)
        blobs.append(encode_frame(cur, prev))
        prev = cur
    n = len(blobs)
    index_off = 32
    data_off = index_off + 4 * (n + 1)
    offsets, pos = [], data_off
    for b in blobs:
        offsets.append(pos)
        pos += len(b)
    offsets.append(pos)
    header = struct.pack("<4sBBHHHHHHHIII", b"UANM", 1, scale, w, h, n, frame_ms, 0, bg, 0,
                         index_off, pos, 0)
    return header + struct.pack(f"<{n + 1}I", *offsets) + b"".join(blobs)


def decode(blob: bytes):
    """Reference decoder (tests): -> list of (h, w) uint16 arrays."""
    magic, ver, scale, w, h, n, frame_ms, _, bg, _, index_off, size, _ = struct.unpack_from(
        "<4sBBHHHHHHHIII", blob)
    assert magic == b"UANM" and ver == 1 and size == len(blob)
    offs = struct.unpack_from(f"<{n + 1}I", blob, index_off)
    buf = np.full(w * h, bg, dtype=np.uint16)
    out = []
    for i in range(n):
        if i == 0:
            buf[:] = bg
        p, c, e = 0, offs[i], offs[i + 1]
        while c < e:
            t, = struct.unpack_from("<H", blob, c)
            c += 2
            op, k = t >> 14, (t & 0x3FFF) + 1
            if op == 0:
                p += k
            elif op == 1:
                buf[p:p + k] = struct.unpack_from("<H", blob, c)[0]
                c += 2
                p += k
            else:
                buf[p:p + k] = np.frombuffer(blob, "<u2", k, c)
                c += 2 * k
                p += k
        out.append(buf.reshape(h, w).copy())
    return out
