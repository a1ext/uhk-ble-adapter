"""JLCPCB production package: Gerber/drill zip (from Altium's OutJob output), BOM and CPL.

    python make_jlc.py

Gerbers come from daughter_board_JLC.OutJob (run it in Altium first); positions and
rotations from pcb_layout.PARTS. JLC places its own EasyEDA footprint (by LCSC code) at the
CPL point / angle, so parts whose EasyEDA origin or 0 deg differ from ours get a correction
(checked against https://easyeda.com/api/products/<LCSC>/components on 2026-10-26).
"""
import csv
import importlib.util
import math
import zipfile
from pathlib import Path

HERE = Path(__file__).parent
OUTPUTS = Path(r"M:\bt-rf-backend\docs\hardware\DaughterBoard\Project Outputs for DaughterBoard")
PROD = HERE / "production"
GERBERS = ["GTL", "GBL", "GTO", "GBO", "GTP", "GBP", "GTS", "GBS", "GM1"]
DRILLS = ["daughter_board-RoundHoles-Plated.TXT", "daughter_board-RoundHoles-NonPlated.TXT",
          "daughter_board-SlotHoles-Plated.TXT"]

# designators -> (comment, LCSC); H1/H2 (THT pin headers) are soldered by hand
PARTS_BOM = {
    "J1": ("U262-161N-4BVC11 USB-C receptacle", "C319148"),
    "F1": ("MF-MSMF110-2 PTC 1.1A", "C89647"),
    "R1": ("5.1k 1% 0603", "C23186"), "R2": ("5.1k 1% 0603", "C23186"),
    "R3": ("15k 1% 0603", "C22809"), "R4": ("15k 1% 0603", "C22809"),
    "C1": ("47uF 10V X5R 1206", "C96123"),
    "C2": ("100nF 50V X7R 0603", "C14663"),
    "C3": ("10uF 16V tantalum B T491B106K016AT", "C110050"),
    "C4": ("1uF 25V tantalum A T491A105K025AT", "C123716"),
}
# EasyEDA footprint vs ours: (origin offset in our footprint frame [mm], extra rotation [deg])
JLC_FIX = {
    "C319148": ((0.0, 1.386), 0),   # EasyEDA origin 1.386 mm toward the pad row (shell legs, pegs)
    "C110050": ((0.0, 0.0), 180),   # EasyEDA tantalum B has pad 1 (+) on the right
}


def main():
    spec = importlib.util.spec_from_file_location("L", HERE / "pcb_layout.py")
    L = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(L)
    PROD.mkdir(exist_ok=True)

    # Gerber + drill zip
    zpath = PROD / "daughter_board_gerber.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for ext in GERBERS:
            z.write(OUTPUTS / f"daughter_board.{ext}", f"daughter_board.{ext}")
        for name in DRILLS:
            z.write(OUTPUTS / name, name)
    print(zpath, len(GERBERS) + len(DRILLS), "files")

    # BOM grouped by LCSC part
    groups = {}
    for ref, (comment, lcsc) in PARTS_BOM.items():
        fp = L.PARTS[ref][0]
        groups.setdefault((comment, fp, lcsc), []).append(ref)
    with open(PROD / "daughter_board_BOM_JLC.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for (comment, fp, lcsc), refs in groups.items():
            w.writerow([comment, ",".join(sorted(refs)), fp, lcsc])

    # CPL: top side, mm from the board origin (= Gerber origin, board bottom-left)
    with open(PROD / "daughter_board_CPL_JLC.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for ref, (comment, lcsc) in PARTS_BOM.items():
            fp, x, y, rot = L.PARTS[ref]
            (dx, dy), drot = JLC_FIX.get(lcsc, ((0.0, 0.0), 0))
            a = math.radians(rot)
            x += dx * math.cos(a) - dy * math.sin(a)
            y += dx * math.sin(a) + dy * math.cos(a)
            w.writerow([ref, f"{x:.3f}mm", f"{y:.3f}mm", "Top", f"{(rot + drot) % 360:g}"])
    print("BOM / CPL written to", PROD)


if __name__ == "__main__":
    main()
