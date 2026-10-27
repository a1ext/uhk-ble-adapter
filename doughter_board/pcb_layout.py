"""Daughter board layout: placement, tracks and vias (mm, Altium board coords,
origin = board bottom-left, Y up), a clearance check and a preview PNG.

Board frame = the LilyGo lying horizontally, display up, its USB-C on the right (+X):
the VBUS header row (H1) is the upper one, H2 the lower one. LilyGo pins 11..14 are
15.9..23.5 mm from its USB-C edge, so the board's right edge is 11.07 mm short of it.
The component side lies on the LilyGo case back (its sockets are flush with the case
and take ~2.3 mm of pin; the header plastic is removed), so only the pin headers sit
under the LilyGo; everything else is on the tab above the LilyGo case (Y > CASE_EDGE),
with the power USB-C facing up at the top edge. 3D-printed shell: shell/PRINT.md.

    python pcb_layout.py            -> check + pcb_preview.png + pcb_ops.txt

Footprint pads come from fp_spec.txt (mils, library coords).
"""
import math
import sys
from pathlib import Path

HERE = Path(__file__).parent
MIL = 0.0254
BOARD_W, BOARD_H = 17.4, 45.0
CASE_EDGE = 26.038 + 4.0   # Y of the LilyGo case wall (VBUS side)
CLEAR = 0.15          # copper-copper target (JLC 2-layer min 0.127)
EDGE = 0.3            # copper-board edge
VIA = (0.6, 0.3)      # size, hole
W_SIG, W_PWR = 0.2, 0.5

# The core (J1, CC / D resistors, F1, caps) in its own frame, J1 facing -Y; core_xy() puts it
# on the tab turned by 180 degrees.
# designator: footprint, x, y, rotation
CORE_PARTS = {
    "J1": ("USB_C_Receptacle_XKB_U262-16XN-4BVC11", 12, 3.7, 0),
    "F1": ("Fuse_1812_4532Metric", 20.3, 11.3, 270),
    "R1": ("R_0603_1608Metric", 9.15, 11.3, 90),
    "R4": ("R_0603_1608Metric", 10.65, 11.3, 90),
    "R3": ("R_0603_1608Metric", 12.25, 11.3, 90),
    "R2": ("R_0603_1608Metric", 13.75, 11.3, 90),
    "C3": ("CP_EIA-3528-21_Kemet-B", 20.22, 7.1, 180),
    "C1": ("C_1206_3216Metric", 20.22, 4.65, 180),
    "C4": ("CP_EIA-3216-18_Kemet-A", 20.22, 2.705, 180),
    "C2": ("C_0603_1608Metric", 20.22, 1.185, 180),
}

NETS = {
    "VBUS_IN": ["J1.A4", "J1.A9", "J1.B4", "J1.B9", "F1.1"],
    "+5V": ["F1.2", "C1.1", "C2.1", "C3.1", "C4.1", "H1.2", "H1.3"],
    "GND": ["J1.A1", "J1.A12", "J1.B1", "J1.B12", "J1.SH", "R1.2", "R2.2", "R3.2", "R4.2",
            "C1.2", "C2.2", "C3.2", "C4.2", "H1.1", "H2.1"],
    "CC1": ["J1.A5", "R1.1"],
    "CC2": ["J1.B5", "R2.1"],
    "USB_DP": ["J1.A6", "J1.B6", "R3.1"],
    "USB_DN": ["J1.A7", "J1.B7", "R4.1"],
}

# tracks: (net, layer, width, [points])  layer T = top, B = bottom
CORE_TRACKS = [
    # VBUS: right pad straight to F1.1 (top), left pad -> vias -> bottom along the top edge
    ("VBUS_IN", "T", W_PWR, [(14.4, 7.4), (14.4, 8.3), (15, 8.9), (15, 13.437), (18.6, 13.437)]),
    ("VBUS_IN", "T", W_PWR, [(15, 13.437), (15, 13.8)]),
    ("VBUS_IN", "T", W_PWR, [(9.6, 7.4), (9.6, 8.1), (7.8, 9.9)]),
    ("VBUS_IN", "B", W_PWR, [(8.4, 9.3), (7.8, 9.9), (7.2, 10.5), (7.2, 13.8), (15, 13.8)]),
    # +5V: F1.2 -> cap bus at x 23.0
    ("+5V", "T", 0.6, [(20.3, 9.163), (23.0, 9.163)]),
    ("+5V", "T", 0.6, [(23.0, 9.163), (23.0, 1.185), (20.995, 1.185)]),
    ("+5V", "T", 0.6, [(23.0, 7.1), (21.76, 7.1)]),
    ("+5V", "T", 0.6, [(23.0, 4.65), (21.695, 4.65)]),
    ("+5V", "T", 0.6, [(23.0, 2.705), (21.572, 2.705)]),
    # GND: J1 GND pads to the shield pads, caps to the shield / vias
    ("GND", "T", 0.4, [(8.8, 7.2), (7.68, 7.2)]),
    ("GND", "T", 0.4, [(15.2, 7.2), (16.32, 7.2)]),
    ("GND", "T", 0.5, [(18.68, 7.0), (16.32, 7.0)]),
    ("GND", "T", 0.5, [(18.745, 4.65), (17.6, 4.65)]),
    ("GND", "T", 0.5, [(18.868, 2.705), (16.32, 2.705)]),
    ("GND", "T", 0.5, [(19.445, 1.185), (18.3, 1.185)]),
    # resistor GND pads to vias
    ("GND", "T", 0.4, [(9.15, 12.125), (9.15, 13.0)]),
    ("GND", "T", 0.4, [(10.65, 12.125), (10.65, 13.0)]),
    ("GND", "T", 0.4, [(12.25, 12.125), (12.25, 13.0)]),
    ("GND", "T", 0.4, [(13.75, 12.125), (13.75, 13.0)]),
    # CC1 / CC2
    ("CC1", "T", W_SIG, [(10.75, 7.4), (10.75, 8.01), (9.15, 9.61), (9.15, 10.475)]),
    ("CC2", "T", W_SIG, [(13.75, 7.4), (13.75, 10.475)]),
    # D-: B7 -> via V1 -> R4.1 ; A7 -> via V2 ; V1-V2 on the bottom
    ("USB_DN", "T", W_SIG, [(11.25, 7.4), (11.25, 8.2), (10.65, 8.8), (10.65, 10.475)]),
    ("USB_DN", "T", W_SIG, [(12.25, 7.4), (12.25, 9.3)]),
    ("USB_DN", "B", W_SIG, [(10.65, 9.1), (10.85, 9.3), (12.25, 9.3)]),
    # D+: A6 left of V2, B6 right of V2, both into R3.1
    ("USB_DP", "T", W_SIG, [(11.75, 7.4), (11.75, 8.5), (11.45, 8.8), (11.45, 10.475), (12.25, 10.475)]),
    ("USB_DP", "T", W_SIG, [(12.75, 7.4), (12.75, 8.5), (13, 8.75), (13, 10.475), (12.25, 10.475)]),
]

CORE_VIAS = [  # net, x, y
    ("VBUS_IN", 8.4, 9.3), ("VBUS_IN", 7.8, 9.9), ("VBUS_IN", 15, 13.8),
    ("USB_DN", 10.65, 9.1), ("USB_DN", 12.25, 9.3),
    ("GND", 9.15, 13.0), ("GND", 10.65, 13.0), ("GND", 12.25, 13.0), ("GND", 13.75, 13.0),
    ("GND", 17.6, 4.65), ("GND", 18.3, 1.185),
]

CORE_X0, CORE_Y0 = 24.0, 45.025   # J1 front on the top edge, bus 0.7 mm from the left edge


def core_xy(x, y):
    return round(CORE_X0 - x, 4), round(CORE_Y0 - y, 4)


PARTS = {r: (fp, *core_xy(x, y), (rot + 180) % 360) for r, (fp, x, y, rot) in CORE_PARTS.items()}
# pin headers onto LilyGo pins 11-14 (pin 1 = LilyGo pin 11); the pins run toward the LilyGo USB end (+X)
PARTS["H1"] = ("PinHeader_1x04_P2.54mm_Vertical", 4.933, 24.449, 90)
PARTS["H2"] = ("PinHeader_1x04_P2.54mm_Vertical", 4.933, 1.589, 90)

TRACKS = [(n, lay, w, [core_xy(*p) for p in pts]) for n, lay, w, pts in CORE_TRACKS] + [
    # +5V from the cap bus down the left edge, over the case wall, above H1.1 to H1.2 / H1.3
    ("+5V", "T", 0.6, [core_xy(23.0, 9.163), (1.0, 26.2), (7.473, 26.2), (7.473, 24.449), (10.013, 24.449)]),
]
VIAS = [(n, *core_xy(x, y)) for n, x, y in CORE_VIAS]


def load_footprints():
    fps, cur = {}, None
    for line in (HERE / "fp_spec.txt").read_text(encoding="utf-8").splitlines():
        f = line.split("|")
        if f[0] == "FOOTPRINT":
            cur = fps.setdefault(f[1], {"pads": [], "court": []})
        elif f[0] == "PAD" and cur is not None:
            name, x, y, rot, layer, plated, hole = f[1], float(f[2]), float(f[3]), float(f[4]), f[5], f[6], float(f[7])
            htype, hwidth, hrot, sx, sy = int(f[8]), float(f[9]), float(f[10]), float(f[11]), float(f[12])
            cur["pads"].append(dict(name=name, x=x * MIL, y=y * MIL, sx=sx * MIL, sy=sy * MIL, layer=layer,
                                    hole=hole * MIL, slot=hwidth * MIL if htype == 2 else 0))
        elif f[0] == "TRACK" and cur is not None and f[6] in ("Mechanical15", "Mechanical Layer 15"):
            cur["court"].append([float(v) * MIL for v in f[1:5]])
    return fps


def rot(x, y, deg):
    a = math.radians(deg)
    return x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a)


def placed_pads(fps):
    """-> list of dict(ref, name, net, layers, rect=(x0,y0,x1,y1), round, hole)"""
    pin_net = {p: n for n, pins in NETS.items() for p in pins}
    out = []
    for ref, (fp, cx, cy, r) in PARTS.items():
        for p in fps[fp]["pads"]:
            px, py = rot(p["x"], p["y"], r)
            sx, sy = (p["sx"], p["sy"]) if r % 180 == 0 else (p["sy"], p["sx"])
            layers = {"T"} if p["layer"] in ("TopLayer", "Top Layer") else {"T", "B"}
            net = pin_net.get(f"{ref}.{p['name']}") if p["name"] else None
            out.append(dict(ref=ref, name=p["name"], net=net, layers=layers,
                            x=cx + px, y=cy + py, sx=sx, sy=sy, hole=p["hole"], slot=p["slot"]))
    return out


def courtyards(fps):
    out = {}
    for ref, (fp, cx, cy, r) in PARTS.items():
        pts = []
        for x1, y1, x2, y2 in fps[fp]["court"]:
            pts += [rot(x1, y1, r), rot(x2, y2, r)]
        xs, ys = [cx + p[0] for p in pts], [cy + p[1] for p in pts]
        out[ref] = (min(xs), min(ys), max(xs), max(ys))
    return out


# --- geometry: everything as segments with a radius ("capsules") or rectangles ---
def seg_dist(p, a, b):
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def seg_seg(a, b, c, d):
    def ccw(p, q, r):
        return (r[1] - p[1]) * (q[0] - p[0]) - (q[1] - p[1]) * (r[0] - p[0])
    if (ccw(a, c, d) * ccw(b, c, d) < 0) and (ccw(a, b, c) * ccw(a, b, d) < 0):
        return 0.0
    return min(seg_dist(a, c, d), seg_dist(b, c, d), seg_dist(c, a, b), seg_dist(d, a, b))


def pad_shape(p):
    """pad -> (kind, data): rect (x0,y0,x1,y1) or capsule (a, b, r)"""
    if p["sx"] == p["sy"] and p["hole"] > 0 and not p["slot"]:
        return ("cap", ((p["x"], p["y"]), (p["x"], p["y"]), p["sx"] / 2))
    if p["slot"] or (p["hole"] > 0):  # oval: capsule along the long side
        r = min(p["sx"], p["sy"]) / 2
        if p["sy"] > p["sx"]:
            h = p["sy"] / 2 - r
            return ("cap", ((p["x"], p["y"] - h), (p["x"], p["y"] + h), r))
        h = p["sx"] / 2 - r
        return ("cap", ((p["x"] - h, p["y"]), (p["x"] + h, p["y"]), r))
    return ("rect", (p["x"] - p["sx"] / 2, p["y"] - p["sy"] / 2, p["x"] + p["sx"] / 2, p["y"] + p["sy"] / 2))


def rect_seg_dist(rc, a, b):
    x0, y0, x1, y1 = rc
    inside = lambda q: x0 <= q[0] <= x1 and y0 <= q[1] <= y1
    if inside(a) or inside(b):
        return 0.0
    corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    return min(seg_seg(a, b, corners[i], corners[(i + 1) % 4]) for i in range(4))


def shape_dist(s1, s2):
    k1, d1 = s1; k2, d2 = s2
    if k1 == "cap" and k2 == "cap":
        return seg_seg(d1[0], d1[1], d2[0], d2[1]) - d1[2] - d2[2]
    if k1 == "rect" and k2 == "cap":
        return rect_seg_dist(d1, d2[0], d2[1]) - d2[2]
    if k1 == "cap" and k2 == "rect":
        return rect_seg_dist(d2, d1[0], d1[1]) - d1[2]
    # rect-rect
    dx = max(d2[0] - d1[2], d1[0] - d2[2], 0)
    dy = max(d2[1] - d1[3], d1[1] - d2[3], 0)
    return math.hypot(dx, dy)


def copper(pads):
    """-> list of (label, net, layer, shape)"""
    items = []
    for p in pads:
        for lay in p["layers"]:
            items.append((f"{p['ref']}.{p['name'] or 'NPTH'}", p["net"] if p["name"] else "NPTH", lay, pad_shape(p)))
    for i, (net, lay, w, pts) in enumerate(TRACKS):
        for a, b in zip(pts, pts[1:]):
            items.append((f"T{i}:{net}", net, lay, ("cap", (a, b, w / 2))))
    for net, x, y in VIAS:
        for lay in "TB":
            items.append((f"V:{net}@{x},{y}", net, lay, ("cap", ((x, y), (x, y), VIA[0] / 2))))
    return items


def check(pads, court):
    errs = []
    items = copper(pads)
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            l1, n1, y1, s1 = items[i]
            l2, n2, y2, s2 = items[j]
            if y1 != y2 or (n1 == n2 and n1 not in (None, "NPTH")):
                continue
            if l1.split(".")[0] == l2.split(".")[0] and l1[0] not in "TV" and n1 is None and n2 is None:
                continue
            d = shape_dist(s1, s2)
            if d < CLEAR - 1e-6:
                errs.append(f"clearance {d:.3f} {y1}: {l1} [{n1}] / {l2} [{n2}]")
    # board edge
    for l, n, lay, s in items:
        k, d = s
        if k == "cap":
            xs = [d[0][0], d[1][0]]; ys = [d[0][1], d[1][1]]; r = d[2]
            box = (min(xs) - r, min(ys) - r, max(xs) + r, max(ys) + r)
        else:
            box = d
        m = min(box[0], box[1], BOARD_W - box[2], BOARD_H - box[3])
        if m < EDGE and not l.startswith("J1"):
            errs.append(f"edge {m:.3f}: {l}")
    # connectivity: every net's pads joined through tracks / vias (union-find on touching copper)
    by_net = {}
    for it in items:
        by_net.setdefault(it[1], []).append(it)
    for net, pins in NETS.items():
        its = [it for it in by_net.get(net, [])]
        parent = list(range(len(its)))
        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a
        for a in range(len(its)):
            for b in range(a + 1, len(its)):
                la, lb = its[a], its[b]
                same_obj = la[0] == lb[0] and la[0][0] in "V" or (la[0] == lb[0] and not la[0].startswith("T"))
                if (la[2] == lb[2] and shape_dist(la[3], lb[3]) <= 1e-6) or same_obj:
                    parent[find(a)] = find(b)
        groups = {}
        for k, it in enumerate(its):
            if not it[0].startswith(("T", "V")):
                groups.setdefault(find(k), set()).add(it[0])
        if len(groups) > 1 and net != "GND":
            errs.append(f"net {net} split: " + " | ".join(",".join(sorted(g)) for g in groups.values()))
        elif net == "GND":
            print("GND islands (joined by the bottom pour):", len(groups))
    for ref, (x0, y0, x1, y1) in court.items():
        if not ref.startswith("H") and y0 < CASE_EDGE + 0.3:
            errs.append(f"{ref} courtyard reaches under the LilyGo case (y {y0:.2f})")
    # courtyards
    refs = list(court)
    for i in range(len(refs)):
        for j in range(i + 1, len(refs)):
            a, b = court[refs[i]], court[refs[j]]
            ox = min(a[2], b[2]) - max(a[0], b[0])
            oy = min(a[3], b[3]) - max(a[1], b[1])
            if ox > 0 and oy > 0:
                print(f"courtyard overlap {refs[i]}/{refs[j]}: {ox:.2f} x {oy:.2f}")
    return errs


def preview(pads, court, path):
    from PIL import Image, ImageDraw
    S = 40
    img = Image.new("RGB", (int(BOARD_W * S) + 40, int(BOARD_H * S) + 40), (20, 20, 24))
    d = ImageDraw.Draw(img)
    T = lambda x, y: (20 + x * S, 20 + (BOARD_H - y) * S)
    d.rectangle([T(0, BOARD_H), T(BOARD_W, 0)], outline=(220, 220, 0), width=2)
    col = {"T": (200, 60, 60), "B": (60, 90, 220)}
    for net, lay, w, pts in TRACKS:
        if lay == "B":
            d.line([T(*p) for p in pts], fill=col[lay], width=max(1, int(w * S)), joint="curve")
    for p in pads:
        x0, y0, x1, y1 = p["x"] - p["sx"] / 2, p["y"] - p["sy"] / 2, p["x"] + p["sx"] / 2, p["y"] + p["sy"] / 2
        c = (190, 150, 60) if p["name"] else (90, 90, 90)
        d.rectangle([T(x0, y1), T(x1, y0)], fill=c)
        if p["hole"]:
            r = p["hole"] / 2
            d.ellipse([T(p["x"] - r, p["y"] + r), T(p["x"] + r, p["y"] - r)], fill=(0, 0, 0))
    for net, lay, w, pts in TRACKS:
        if lay == "T":
            d.line([T(*p) for p in pts], fill=col[lay], width=max(1, int(w * S)), joint="curve")
    for net, x, y in VIAS:
        r = VIA[0] / 2
        d.ellipse([T(x - r, y + r), T(x + r, y - r)], fill=(160, 160, 160))
        r = VIA[1] / 2
        d.ellipse([T(x - r, y + r), T(x + r, y - r)], fill=(0, 0, 0))
    for ref, (x0, y0, x1, y1) in court.items():
        d.rectangle([T(x0, y1), T(x1, y0)], outline=(80, 200, 120))
        d.text(T(x0 + 0.1, y1 - 0.1), ref, fill=(120, 255, 160))
    img.save(path)


def main():
    fps = load_footprints()
    pads = placed_pads(fps)
    court = courtyards(fps)
    errs = check(pads, court)
    for e in errs:
        print("ERR", e)
    preview(pads, court, HERE / "pcb_preview.png")
    print(f"{len(errs)} errors")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())


def write_ops(path):
    """Ops for the Altium apply scripts: pad nets, tracks, vias (mm, '.' decimals)."""
    lines = [f"{pin}={net}" for net, pins in NETS.items() for pin in pins]
    Path(path, "padnets.txt").write_text("\n".join(lines) + "\n")
    ops = []
    for net, lay, w, pts in TRACKS:
        for a, b in zip(pts, pts[1:]):
            ops.append(f"T|{lay}|{net}|{w}|{a[0]}|{a[1]}|{b[0]}|{b[1]}")
    for net, x, y in VIAS:
        ops.append(f"V|{net}|{x}|{y}|{VIA[0]}|{VIA[1]}")
    Path(path, "route_ops.txt").write_text("\n".join(ops) + "\n")
    print(f"{len(lines)} pad nets, {len(ops)} route ops -> {path}")
