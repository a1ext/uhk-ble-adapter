"""Finds designator positions: clear of mask openings (pad + 0.05) by 0.1 mm, of other
silk by 0.26 mm, inside the board by 0.25 mm; prefers spots near the part, outside other
parts' courtyards. Writes silk_ops.txt (ref|x|y|rot = anchor in mm)."""
import math, sys
import importlib.util
from pathlib import Path
spec = importlib.util.spec_from_file_location("L", Path(__file__).with_name("pcb_layout.py"))
L = importlib.util.module_from_spec(spec); spec.loader.exec_module(L)

# measured Altium boxes relative to the anchor at rotation 0 (0.8 mm / 0.15 mm stroke)
WIDE = {"C1": 1.366, "F1": 1.366, "R1": 1.366, "J1": 1.366}
BOX = lambda ref: (-0.15, -0.15, WIDE.get(ref, 1.633) - 0.15, 0.95)

fps = L.load_footprints()
pads = L.placed_pads(fps)
court = L.courtyards(fps)
masks = []
for p in pads:
    e = 0.05
    masks.append((p["x"] - p["sx"] / 2 - e, p["y"] - p["sy"] / 2 - e, p["x"] + p["sx"] / 2 + e, p["y"] + p["sy"] / 2 + e))
silk = []
cur = None
lines_by_fp = {}
for line in Path(__file__).with_name("fp_spec.txt").read_text().splitlines():
    f = line.split("|")
    if f[0] == "FOOTPRINT": cur = lines_by_fp.setdefault(f[1], [])
    elif f[0] == "TRACK" and f[6] in ("TopOverlay", "Top Overlay"): cur.append([float(v) * L.MIL for v in f[1:6]])
for ref, (fp, cx, cy, r) in L.PARTS.items():
    for x1, y1, x2, y2, w in lines_by_fp.get(fp, []):
        a = L.rot(x1, y1, r); b = L.rot(x2, y2, r)
        a, b = (cx + a[0], cy + a[1]), (cx + b[0], cy + b[1])
        # removed on the board: the long horizontal lines of C3/C4 (they ran into the neighbours' pads)
        if ref in ("C3", "C4") and abs(a[1] - b[1]) < 1e-6:
            continue
        silk.append((a, b, w / 2, ref))

def rbox(ref, x, y, rot):
    l, b, r_, t = BOX(ref)
    pts = [L.rot(px, py, rot) for px, py in ((l, b), (r_, b), (r_, t), (l, t))]
    xs = [x + p[0] for p in pts]; ys = [y + p[1] for p in pts]
    return (min(xs), min(ys), max(xs), max(ys))

def rr_dist(a, b):
    dx = max(b[0] - a[2], a[0] - b[2], 0); dy = max(b[1] - a[3], a[1] - b[3], 0)
    return math.hypot(dx, dy) if (dx or dy) else -1

def ok(box, placed):
    if box[0] < 0.25 or box[1] < 0.25 or box[2] > L.BOARD_W - 0.25 or box[3] > L.BOARD_H - 0.25:
        return False
    if any(rr_dist(box, m) < 0.1 for m in masks): return False
    if any(L.rect_seg_dist(box, a, b) - w < 0.26 for a, b, w, _ in silk): return False
    if any(rr_dist(box, q) < 0.26 for q in placed.values()): return False
    return True

ORDER = ["C2", "C4", "C1", "C3", "F1", "H1", "H2", "R1", "R4", "R3", "R2", "J1"]
placed, out = {}, []
for ref in ORDER:
    fp, cx, cy, r = L.PARTS[ref]
    best = None
    for rot in (0, 90):
        for ix in range(-60, 61):
            for iy in range(-60, 61):
                x, y = cx + ix * 0.1, cy + iy * 0.1
                box = rbox(ref, x, y, rot)
                if not ok(box, placed): continue
                bx, by = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
                d = math.hypot(bx - cx, by - cy) + (0.6 if rot else 0)
                if any(k != ref and rr_dist(box, c) < 0 for k, c in court.items()): d += 3
                if best is None or d < best[0]: best = (d, x, y, rot, box)
    if best is None:
        print("NO SPOT", ref); continue
    d, x, y, rot, box = best
    placed[ref] = box
    out.append(f"{ref}|{x:.3f}|{y:.3f}|{rot}")
    print(f"{ref}: anchor ({x:.2f},{y:.2f}) rot {rot} box {tuple(round(v,2) for v in box)} score {d:.2f}")
Path(sys.argv[1]).write_text("\n".join(out) + "\n")
