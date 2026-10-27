"""Checks hand-placed designators (WANT: anchor x, y, rotation, text height) against
mask openings (>= 0.1 mm), other silk and each other (>= 0.15 mm), board edge (0.25 mm);
writes silk_ops.txt for the Altium apply script."""
import sys
from pathlib import Path
src = Path(__file__).with_name("silk_place.py").read_text().split("ORDER = ")[0]
ns = {"__file__": str(Path(__file__).with_name("silk_place.py").resolve())}
exec(compile(src, "silk_place", "exec"), ns)
L, rr_dist, masks, silk, BOX = ns["L"], ns["rr_dist"], ns["masks"], ns["silk"], ns["BOX"]

WANT = {
    "J1": (11.5, 40.93, 0, 0.8),       # inside the receptacle outline
    "R2": (10.65, 35.28, 90, 0.8),     # R1..R4: vertical, above each resistor
    "R3": (12.15, 35.28, 90, 0.8),
    "R4": (13.75, 35.28, 90, 0.8),
    "R1": (15.25, 35.28, 90, 0.8),
    "F1": (3.17, 33.33, 0, 0.8),       # between the fuse pads
    "C3": (4.33, 37.17, 90, 0.8),
    "C1": (3.28, 39.98, 0, 0.8),
    "C4": (3.5, 42.875, 270, 0.65),
    "C2": (5.38, 43.44, 0, 0.8),
    "H1": (4.23, 25.65, 0, 0.8),
    "H2": (4.23, 2.79, 0, 0.8),
}
# pin names on the bottom overlay: rotation 90, mirrored, 0.8 / 0.15 mm; the box spans x-0.95..x+0.15
# and grows downward from the anchor. H1 labels hang below H1 (top edge 0.15 mm from the pad masks),
# H2 labels stand above H2 (bottom edge 0.15 mm from the masks).
BOTTOM_LABELS = [("GND", 5.333, 23.25), ("VBUS", 7.873, 23.25), ("VBUS", 10.413, 23.25), ("IO16", 12.953, 23.25),
                 ("GND", 5.333, 4.923), ("3V3", 7.873, 4.923), ("VBAT", 10.413, 5.722), ("IO39", 12.953, 5.589)]
EXTRA_SILK = [((3.17, 36.975), (3.17, 38.875), 0.06)]  # C3 polarity bar (free track)

def box_of(ref, x, y, rot, h):
    l, b, r, t = BOX(ref)
    s = h / 0.8
    r, t = (r + 0.15) * s - 0.15, (t + 0.15) * s - 0.15
    pts = [L.rot(px, py, rot) for px, py in ((l, b), (r, b), (r, t), (l, t))]
    xs = [x + p[0] for p in pts]; ys = [y + p[1] for p in pts]
    return (min(xs), min(ys), max(xs), max(ys))

bad, boxes = 0, {}
for ref, (x, y, rot, h) in WANT.items():
    b = box_of(ref, x, y, rot, h); boxes[ref] = b
    m = min(rr_dist(b, q) for q in masks)
    s = min(L.rect_seg_dist(b, a, c) - w for a, c, w, *_ in silk + EXTRA_SILK)
    edge = min(b[0], b[1], L.BOARD_W - b[2], L.BOARD_H - b[3])
    ok = m >= 0.1 and s >= 0.15 and edge >= 0.25
    bad += not ok
    print(f"{ref}: mask {m:.2f} silk {s:.2f} edge {edge:.2f} {'ok' if ok else 'BAD'}")
for i, a in enumerate(boxes):
    for bref in list(boxes)[i + 1:]:
        if rr_dist(boxes[a], boxes[bref]) < 0.15:
            print("text-text", a, bref); bad += 1
for a, c, w in EXTRA_SILK:
    m = min(L.rect_seg_dist(q, a, c) - w for q in masks)
    print(f"bar: mask {m:.2f}"); bad += m < 0.1
Path(sys.argv[1]).write_text("\n".join(f"{r}|{x}|{y}|{rot}|{h}" for r, (x, y, rot, h) in WANT.items()) + "\n")
pads = L.placed_pads(L.load_footprints())
for a, c, w, ref in silk:
    for p in pads:
        if p["ref"] == ref:
            continue
        e = 0.05
        m = (p["x"] - p["sx"] / 2 - e, p["y"] - p["sy"] / 2 - e, p["x"] + p["sx"] / 2 + e, p["y"] + p["sy"] / 2 + e)
        d = L.rect_seg_dist(m, a, c) - w
        if d < 0.1:
            print(f"silk of {ref} {a}-{c} vs pad {p['ref']}.{p['name']}: {d:.2f}"); bad += 1
print("problems:", bad)
