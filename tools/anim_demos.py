"""Procedural standby animations for the adapter (frames for uanm.encode)."""
import random

import numpy as np
from PIL import Image, ImageDraw, ImageFont

NEON = (57, 255, 20)


def matrix_rain(width=268, height=120, frames=96, cell=(8, 10), trail=9, seed=7):
    """Neon-green digital rain, a seamless loop: every column's drop covers a whole
    number of its own cycles in `frames`, and the glyph flicker repeats with it."""
    rnd = random.Random(seed)
    cw, ch = cell
    cols, rows = width // cw, height // ch
    glyphs = "0123456789ABCDEF$#%&*+=<>"
    try:
        font = ImageFont.load_default(size=ch)
    except TypeError:                       # older Pillow: bitmap font only
        font = ImageFont.load_default()

    # brightness levels: 0 = head (near white), then the trail fading to dark green
    levels = [(210, 255, 210)] + [
        tuple(int(c * (1 - i / trail) ** 1.6) for c in NEON) for i in range(trail)]
    sprites = {}
    for gi, g in enumerate(glyphs):
        for li, col in enumerate(levels):
            im = Image.new("RGB", (cw, ch))
            ImageDraw.Draw(im).text((cw // 2, ch // 2), g, fill=col, font=font, anchor="mm")
            sprites[gi, li] = np.asarray(im)

    columns = []
    for c in range(cols):
        cycle = rows + trail + rnd.randint(2, rows)     # cells, incl. the gap above
        laps = rnd.choice((1, 1, 2))                    # whole laps per loop
        columns.append({"cycle": cycle, "speed": cycle * laps / frames,
                        "phase": rnd.uniform(0, cycle), "flick": rnd.choice((8, 12, 16, 24))})
    glyph_of = [[rnd.randrange(len(glyphs)) for _ in range(rows)] for _ in range(cols)]

    out = []
    for t in range(frames):
        a = np.zeros((rows * ch, cols * cw, 3), dtype=np.uint8)
        for c, col in enumerate(columns):
            head = int(col["phase"] + col["speed"] * t) % col["cycle"]
            for r in range(rows):
                d = head - r
                if 0 <= d <= trail:
                    # glyphs flicker every 'flick' frames; flick divides frames: still a loop
                    gi = (glyph_of[c][r] + (t // col["flick"]) * 7 * (d > 0)) % len(glyphs)
                    a[r * ch:(r + 1) * ch, c * cw:(c + 1) * cw] = sprites[gi, min(d, trail)]
        img = Image.new("RGB", (width, height))
        img.paste(Image.fromarray(a), ((width - cols * cw) // 2, (height - rows * ch) // 2))
        out.append(img)
    return out


DEMOS = {"matrix": matrix_rain}
