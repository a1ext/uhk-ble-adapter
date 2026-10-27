"""Converts KiCad footprints (.kicad_mod) into a create_footprints_batch spec for
the Altium MCP, plus a list of the KiCad STEP models to attach.

    python kicad2altium.py <out_spec.txt> <out_models.txt> <PcbLib path> Lib:Footprint ...

KiCad: mm, Y down. Altium: mils, Y up (y is negated; rotations keep their
visual sense). Layers: F.Cu -> Top Layer, through-hole -> Multi Layer,
F.SilkS -> Top Overlay, F.Fab -> Mechanical Layer 13, F.CrtYd -> Mechanical Layer 15.
Layer names are spelled the way Altium's String2Layer reads them ("TopLayer"
or "Mechanical 13" come out as No Layer).
"""
import math
import re
import sys
from pathlib import Path

KICAD = Path(r"C:\Program Files\KiCad\10.0\share\kicad")
MIL = 1 / 0.0254
GFX_LAYERS = {"F.SilkS": "Top Overlay", "F.Fab": "Mechanical Layer 13", "F.CrtYd": "Mechanical Layer 15"}
SHAPE = {"rect": 2, "roundrect": 9, "circle": 1, "oval": 1}


def parse(text):
    """Minimal S-expression parser -> nested lists of str."""
    tokens = re.findall(r'"(?:[^"\\]|\\.)*"|\(|\)|[^\s()"]+', text)
    stack, cur = [], []
    for t in tokens:
        if t == "(":
            stack.append(cur)
            cur = []
        elif t == ")":
            done, cur = cur, stack.pop()
            cur.append(done)
        else:
            cur.append(t[1:-1] if t.startswith('"') else t)
    return cur[0]


def find(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]


def first(node, key):
    f = find(node, key)
    return f[0] if f else None


def xy(node):
    return float(node[1]), float(node[2])


def m(v):
    return round(v * MIL, 4)


def pt(x, y):  # KiCad mm -> Altium mils, Y up
    return m(x), m(-y)


def layer_of(node):
    lay = first(node, "layer")
    return GFX_LAYERS.get(lay[1]) if lay else None


def width_of(node):
    st = first(node, "stroke")
    w = first(st, "width") if st else first(node, "width")
    return m(float(w[1])) if w else 4


def arc_from_3(p1, p2, p3):
    (x1, y1), (x2, y2), (x3, y3) = p1, p2, p3
    d = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
    ux = ((x1**2 + y1**2) * (y2 - y3) + (x2**2 + y2**2) * (y3 - y1) + (x3**2 + y3**2) * (y1 - y2)) / d
    uy = ((x1**2 + y1**2) * (x3 - x2) + (x2**2 + y2**2) * (x1 - x3) + (x3**2 + y3**2) * (x2 - x1)) / d
    r = math.hypot(x1 - ux, y1 - uy)
    a = lambda x, y: math.degrees(math.atan2(y - uy, x - ux)) % 360
    s, mid, e = a(x1, y1), a(x2, y2), a(x3, y3)
    ccw = lambda frm, to: (to - frm) % 360
    if ccw(s, mid) > ccw(s, e):  # mid not on the CCW path start->end: draw end->start
        s, e = e, s
    return ux, uy, r, s, e


def convert(lib, name, out, models):
    path = KICAD / "footprints" / f"{lib}.pretty" / f"{name}.kicad_mod"
    fp = parse(path.read_text(encoding="utf-8"))
    descr = (first(fp, "descr") or ["", ""])[1].replace("|", "/")[:200]
    out.append(f"FOOTPRINT|{name}|{descr}")
    for p in find(fp, "pad"):
        num, kind, shape = p[1], p[2], p[3]
        at = first(p, "at")
        x, y = pt(float(at[1]), float(at[2]))
        rot = float(at[3]) if len(at) > 3 else 0
        sw, sh = (float(v) for v in first(p, "size")[1:3])
        drill = first(p, "drill")
        hole, htype, hwidth, hrot = 0, 0, 0, 0
        if drill:
            vals = drill[1:]
            if vals and vals[0] == "oval":
                dx, dy = float(vals[1]), float(vals[2])
                htype = 2
                hole, hwidth = m(min(dx, dy)), m(max(dx, dy))
                hrot = 90 if dy > dx else 0
            else:
                hole = m(float(vals[0]))
        layer = "Top Layer" if kind == "smd" else "Multi Layer"
        plated = 0 if kind == "np_thru_hole" else 1
        rr = first(p, "roundrect_rratio")
        pct = str(min(100, round(float(rr[1]) * 200))) if shape == "roundrect" and rr else ""
        out.append(f"PAD|{num}|{x}|{y}|{rot}|{layer}|{plated}|{hole}|{htype}|{hwidth}|{hrot}|"
                   f"{m(sw)}|{m(sh)}|{SHAPE.get(shape, 2)}|{pct}")
    for g in fp:
        if not isinstance(g, list) or g[0] not in ("fp_line", "fp_rect", "fp_circle", "fp_arc", "fp_poly"):
            continue
        lay = layer_of(g)
        if not lay:
            continue
        w = width_of(g)
        if g[0] == "fp_line":
            (x1, y1), (x2, y2) = pt(*xy(first(g, "start"))), pt(*xy(first(g, "end")))
            out.append(f"TRACK|{x1}|{y1}|{x2}|{y2}|{w}|{lay}")
        elif g[0] in ("fp_rect", "fp_poly"):
            if g[0] == "fp_rect":
                (sx, sy), (ex, ey) = xy(first(g, "start")), xy(first(g, "end"))
                pts = [(sx, sy), (ex, sy), (ex, ey), (sx, ey)]
            else:
                pts = [xy(c) for c in first(g, "pts") if isinstance(c, list) and c[0] == "xy"]
            pts = [pt(*q) for q in pts]
            for (x1, y1), (x2, y2) in zip(pts, pts[1:] + pts[:1]):
                out.append(f"TRACK|{x1}|{y1}|{x2}|{y2}|{w}|{lay}")
        elif g[0] == "fp_circle":
            c, e = xy(first(g, "center")), xy(first(g, "end"))
            cx, cy = pt(*c)
            out.append(f"ARC|{cx}|{cy}|{m(math.dist(c, e))}|0|360|{w}|{lay}")
        elif g[0] == "fp_arc":
            p1, p2, p3 = (pt(*xy(first(g, k))) for k in ("start", "mid", "end"))
            cx, cy, r, s, e = arc_from_3(p1, p2, p3)
            out.append(f"ARC|{round(cx, 4)}|{round(cy, 4)}|{round(r, 4)}|{round(s, 3)}|{round(e, 3)}|{w}|{lay}")
    for mod in find(fp, "model"):
        p = mod[1].replace("${KICAD10_3DMODEL_DIR}", str(KICAD / "3dmodels")).replace("/", "\\")
        p = re.sub(r"\.wrl$", ".step", p)
        rot = first(mod, "rotate")
        rz = float(first(rot, "xyz")[3]) if rot else 0
        models.append(f"{name}|{p}|{rz}")


def main():
    out_spec, out_models, pcblib = sys.argv[1:4]
    out, models = [f"FPLIB|{pcblib}"], []
    for ref in sys.argv[4:]:
        lib, name = ref.split(":", 1)
        convert(lib, name, out, models)
    Path(out_spec).write_text("\n".join(out) + "\n", encoding="utf-8")
    Path(out_models).write_text("\n".join(models) + "\n", encoding="utf-8")
    print(f"{len(sys.argv) - 4} footprints, {len(out) - 1} records, {len(models)} models")


if __name__ == "__main__":
    main()
