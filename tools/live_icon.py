"""Icon of the live spectrum app: neon bars like the adapter draws them, pink
(bass) to teal (treble); grey while not streaming."""
from PIL import Image, ImageDraw

HEIGHTS = [0.78, 0.62, 0.86, 0.55, 0.70, 0.42, 0.50]
COLOURS = [(255, 0, 128), (230, 0, 190), (170, 0, 255), (110, 40, 255), (40, 70, 255),
           (0, 120, 230), (0, 170, 190)]


def make(active=True, size=256) -> Image.Image:
    s = 256
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=48, fill=(10, 10, 14, 255))
    w, gap, base = 24, 8, s - 40
    x = (s - (len(HEIGHTS) * (w + gap) - gap)) // 2
    for h, c in zip(HEIGHTS, COLOURS):
        if not active:
            c = (90, 90, 100)
        top = base - int(h * (s - 80))
        d.rectangle([x, top, x + w - 1, base], fill=c + (255,))
        d.rectangle([x, top - 14, x + w - 1, top - 9], fill=(235, 235, 255, 255) if active else (130, 130, 140, 255))
        x += w + gap
    return img if size == s else img.resize((size, size), Image.LANCZOS)


def save_ico(path):
    make().save(path, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
