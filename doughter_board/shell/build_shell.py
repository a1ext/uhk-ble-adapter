"""Fusion 360 script: (re)builds the UHK BLE adapter shell - Lid + Tray - from
parameters, in the open uhk_ble_shell_assembly.f3d (LilyGo case + daughter board
reference for the interference check), and exports the STL / STEP / f3d files
next to this script.

Run it from Fusion (Utilities > Scripts and Add-Ins > + > this file) or through
the Fusion MCP. An existing UHK_BLE_Shell component is deleted first.

2026-11-09, after JLCPCB's wall thickness review (SLA, Resin 8001 transparent,
min 0.8 mm, 1.5 optimal): the tray's screw head counterbores (dia 2.4) were 0.3 /
0.4 mm from its side walls and the dia 3.0 bosses left a 0.3 mm ring around
them. Now: tray and lid tab widened (y 6.8..32.7 instead of 7.8..31.8, the
screws stay where the daughter board allows them), tray bosses dia 4.0, the
counterbore only as deep as the floor (1.2 mm). Lid (min 0.9 mm wanted): the
case screw head seat moved up to Z 6.9, above the daughter board, with dia 4.8
bosses around the dia 2.4 counterbores from there to the ceiling (the dia 3.4
bosses left a 0.5 mm ring); pilot holes 3.4 deep (1.0 mm to the top).
"""
import math
import os

import adsk.core
import adsk.fusion

MM = 0.1   # Fusion API units are cm
EPS = 1e-4
OUT = os.path.dirname(os.path.abspath(__file__))

# ---- parameters (mm, LilyGo board coordinates: USB-C at y = 0, Z up from the LilyGo PCB) ----
CASE_X0, CASE_Y0, CASE_X1, CASE_Y1, CASE_R = -0.69, -1.09, 26.83, 55.36, 4.46  # case back outline at Z 4.5
LID_Z0, LID_H, LID_WALL = 5.0, 4.4, 1.6
TAB_X0, TAB_X1 = -20.76, 2.0            # the tab over the daughter board's USB-C end
TAB_Y0, TAB_Y1 = 6.8, 32.7              # was 7.8 .. 31.8
TRAY_X1, TRAY_H, TRAY_WALL = -3.6, 5.0, 1.2
CASE_SCREWS = [(4.26, 9.12, 3.4), (21.71, 9.07, 3.4), (22.34, 52.47, 4.5)]   # x, y, boss dia
TRAY_SCREWS = [(-12.0, 9.3), (-12.0, 30.2)]   # as far out as the board (y 11.07..28.47) allows the lid bosses
LID_BOSS_D, LID_PILOT_D = 3.0, 1.0
TRAY_BOSS_D, TRAY_HOLE_D, TRAY_HEAD_D, TRAY_HEAD_DEPTH = 4.0, 1.4, 2.4, 1.2   # was 3.0 / 1.4 / 2.4 x 2.0
CASE_HOLE_D, CASE_HEAD_D, CASE_HEAD_TO_Z = 1.4, 2.4, 6.9   # head seat was 5.8: 0.5 mm ring around it
CASE_BOSS_TOP_D = 4.8   # boss around the head counterbore, Z CASE_HEAD_TO_Z .. ceiling (over the board, 1.2 mm ring)
LID_PILOT_DEPTH = 3.4   # 1.0 mm left under the lid top (was 3.6 -> 0.8); a M1.2x6 ends at Z 7.2
J1_Y, J1_Z = 16.473, 3.35               # daughter board USB-C receptacle mouth centre
USB_W, USB_H, USB_R = 13.0, 7.4, 2.0    # plug opening through both end walls
PADS_Y = (13.5, 26.0)                   # board press pads on the lid tab ceiling
SLOT = (24.5, 10.87, 28.0, 28.67, 1.8)  # board edge slot in the H2-side lid wall


def run(_context: str):
    app = adsk.core.Application.get()
    ui = app.userInterface
    design = adsk.fusion.Design.cast(app.activeProduct)
    root = design.rootComponent
    log = []

    def say(*a):
        s = ' '.join(str(x) for x in a)
        log.append(s)
        print(s)

    for i in range(root.occurrences.count - 1, -1, -1):
        o = root.occurrences.item(i)
        if o.component.name == 'UHK_BLE_Shell':
            o.deleteMe()
            say('old shell component deleted')

    occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
    comp = occ.component
    comp.name = 'UHK_BLE_Shell'
    planes = comp.constructionPlanes
    ext = comp.features.extrudeFeatures
    fil = comp.features.filletFeatures
    NEW = adsk.fusion.FeatureOperations.NewBodyFeatureOperation
    JOIN = adsk.fusion.FeatureOperations.JoinFeatureOperation
    CUT = adsk.fusion.FeatureOperations.CutFeatureOperation

    def plane_off(base, d):
        pi = planes.createInput()
        pi.setByOffset(base, adsk.core.ValueInput.createByReal(d * MM))
        return planes.add(pi)

    def plane_z(z):
        return plane_off(comp.xYConstructionPlane, z)

    def P(x, y):
        return adsk.core.Point3D.create(x * MM, y * MM, 0)

    def coll(items):
        c = adsk.core.ObjectCollection.create()
        for i in items:
            c.add(i)
        return c

    def rrect(sk, x0, y0, x1, y1, r):
        L = sk.sketchCurves.sketchLines
        A = sk.sketchCurves.sketchArcs
        L.addByTwoPoints(P(x0 + r, y0), P(x1 - r, y0))
        L.addByTwoPoints(P(x1, y0 + r), P(x1, y1 - r))
        L.addByTwoPoints(P(x1 - r, y1), P(x0 + r, y1))
        L.addByTwoPoints(P(x0, y1 - r), P(x0, y0 + r))
        A.addByCenterStartSweep(P(x1 - r, y0 + r), P(x1 - r, y0), math.pi / 2)
        A.addByCenterStartSweep(P(x1 - r, y1 - r), P(x1, y1 - r), math.pi / 2)
        A.addByCenterStartSweep(P(x0 + r, y1 - r), P(x0 + r, y1), math.pi / 2)
        A.addByCenterStartSweep(P(x0 + r, y0 + r), P(x0, y0 + r), math.pi / 2)

    def rect(sk, x0, y0, x1, y1):
        sk.sketchCurves.sketchLines.addTwoPointRectangle(P(x0, y0), P(x1, y1))

    def extrude(profiles, dist, op, bodies=None):
        inp = ext.createInput(profiles, op)
        inp.setDistanceExtent(False, adsk.core.ValueInput.createByReal(dist * MM))
        if bodies is not None:
            inp.participantBodies = bodies
        return ext.add(inp)

    def circles(plane, specs, name):
        sk = comp.sketches.add(plane)
        sk.name = name
        for x, y, d in specs:
            sk.sketchCurves.sketchCircles.addByCenterRadius(P(x, y), d / 2 * MM)
        return sk

    def circle_profiles(sk):
        out = []
        for c in sk.sketchCurves.sketchCircles:
            best = None
            for p in sk.profiles:
                for lp in p.profileLoops:
                    for cu in lp.profileCurves:
                        if cu.sketchEntity == c and (best is None or p.areaProperties().area < best.areaProperties().area):
                            best = p
            out.append(best)
        return out

    def planar_face_at(body, z):
        best = None
        for f in body.faces:
            bb = f.boundingBox
            if isinstance(f.geometry, adsk.core.Plane) and abs(bb.minPoint.z - z * MM) < EPS and abs(bb.maxPoint.z - z * MM) < EPS:
                if best is None or f.area > best.area:
                    best = f
        return best

    def shell_open(body, z, t):
        si = comp.features.shellFeatures.createInput(coll([planar_face_at(body, z)]), False)
        si.insideThickness = adsk.core.ValueInput.createByReal(t * MM)
        comp.features.shellFeatures.add(si)

    def vertical_edges(body, pts):
        out = adsk.core.ObjectCollection.create()
        for e in body.edges:
            if not isinstance(e.geometry, adsk.core.Line3D):
                continue
            a, b = e.startVertex.geometry, e.endVertex.geometry
            if abs(a.x - b.x) > EPS or abs(a.y - b.y) > EPS:
                continue
            for x, y in pts:
                if abs(a.x - x * MM) < EPS and abs(a.y - y * MM) < EPS:
                    out.add(e)
        return out

    def outer_loop_edges(face, skip=None):
        out = adsk.core.ObjectCollection.create()
        for lp in face.loops:
            if lp.isOuter:
                for e in lp.edges:
                    if not (skip and skip(e)):
                        out.add(e)
        return out

    def do_fillet(edges, r, label, tangent_chain=False):
        if edges.count == 0:
            say(label, 'NO EDGES')
            return False
        fi = fil.createInput()
        fi.addConstantRadiusEdgeSet(edges, adsk.core.ValueInput.createByReal(r * MM), tangent_chain)
        try:
            fil.add(fi)
            say(label, 'ok', edges.count, 'edges R', r)
            return True
        except Exception as ex:
            say(label, 'FAILED', str(ex)[:150])
            return False

    # ---- Lid: case back outline + tab, Z 5.0 .. 9.4, shelled from below ----
    sk = comp.sketches.add(plane_z(LID_Z0))
    sk.name = 'lid_outline'
    rrect(sk, CASE_X0, CASE_Y0, CASE_X1, CASE_Y1, CASE_R)
    rect(sk, TAB_X0, TAB_Y0, TAB_X1, TAB_Y1)
    lid = extrude(coll(list(sk.profiles)), LID_H, NEW).bodies.item(0)
    lid.name = 'Lid'
    shell_open(lid, LID_Z0, LID_WALL)
    lid_top = LID_Z0 + LID_H
    lid_ceiling = lid_top - LID_WALL
    # bosses up to the ceiling
    sk = circles(plane_z(LID_Z0), CASE_SCREWS + [(x, y, LID_BOSS_D) for x, y in TRAY_SCREWS], 'lid_bosses')
    extrude(coll(circle_profiles(sk)), lid_ceiling - LID_Z0, JOIN, [lid])
    # wider boss around each case screw head, above the daughter board (Z 5.0 .. 6.6)
    sk = circles(plane_z(CASE_HEAD_TO_Z), [(x, y, CASE_BOSS_TOP_D) for x, y, _ in CASE_SCREWS], 'lid_head_bosses')
    extrude(coll(circle_profiles(sk)), lid_ceiling - CASE_HEAD_TO_Z, JOIN, [lid])
    # board press pads on the tab: Z 6.75 -> ceiling
    sk = comp.sketches.add(plane_z(6.75))
    sk.name = 'lid_pads'
    for y in PADS_Y:
        rect(sk, -11.5, y - 1.5, -8.5, y + 1.5)
    extrude(coll(list(sk.profiles)), lid_ceiling - 6.75, JOIN, [lid])
    # case screws: through hole, head counterbore from the top down to CASE_HEAD_TO_Z
    sk = circles(plane_z(LID_Z0), [(x, y, CASE_HOLE_D) for x, y, _ in CASE_SCREWS], 'lid_screw_holes')
    extrude(coll(circle_profiles(sk)), LID_H, CUT, [lid])
    sk = circles(plane_z(lid_top), [(x, y, CASE_HEAD_D) for x, y, _ in CASE_SCREWS], 'lid_screw_heads')
    extrude(coll(circle_profiles(sk)), -(lid_top - CASE_HEAD_TO_Z), CUT, [lid])
    # tray screw pilot holes into the tab bosses
    sk = circles(plane_z(LID_Z0), [(x, y, LID_PILOT_D) for x, y in TRAY_SCREWS], 'lid_tray_pilots')
    extrude(coll(circle_profiles(sk)), LID_PILOT_DEPTH, CUT, [lid])
    # board edge slot in the H2-side wall
    sk = comp.sketches.add(plane_z(LID_Z0))
    sk.name = 'lid_board_slot'
    rect(sk, SLOT[0], SLOT[1], SLOT[2], SLOT[3])
    extrude(coll(list(sk.profiles)), SLOT[4], CUT, [lid])

    # ---- Tray: under the tab, Z 0 .. 5.0, shelled from the top ----
    xy = comp.xYConstructionPlane
    sk = comp.sketches.add(xy)
    sk.name = 'tray_outline'
    rect(sk, TAB_X0, TAB_Y0, TRAY_X1, TAB_Y1)
    tray = extrude(coll(list(sk.profiles)), TRAY_H, NEW).bodies.item(0)
    tray.name = 'Tray'
    shell_open(tray, TRAY_H, TRAY_WALL)
    sk = circles(xy, [(x, y, TRAY_BOSS_D) for x, y in TRAY_SCREWS], 'tray_bosses')
    extrude(coll(circle_profiles(sk)), TRAY_H, JOIN, [tray])
    sk = circles(xy, [(x, y, TRAY_HOLE_D) for x, y in TRAY_SCREWS], 'tray_holes')
    extrude(coll(circle_profiles(sk)), TRAY_H, CUT, [tray])
    sk = circles(xy, [(x, y, TRAY_HEAD_D) for x, y in TRAY_SCREWS], 'tray_heads')
    extrude(coll(circle_profiles(sk)), TRAY_HEAD_DEPTH, CUT, [tray])

    # ---- USB-C plug opening through the end walls of both parts ----
    x_cut = TAB_X0 - 2.24
    pl = plane_off(comp.yZConstructionPlane, x_cut)
    sk = comp.sketches.add(pl)
    sk.name = 'usb_opening'
    w, h, r = USB_W, USB_H, USB_R
    y0, y1, z0, z1 = J1_Y - w / 2, J1_Y + w / 2, J1_Z - h / 2, J1_Z + h / 2

    def S(y, z):
        return sk.modelToSketchSpace(adsk.core.Point3D.create(x_cut * MM, y * MM, z * MM))

    L = sk.sketchCurves.sketchLines
    A = sk.sketchCurves.sketchArcs
    L.addByTwoPoints(S(y0 + r, z0), S(y1 - r, z0))
    L.addByTwoPoints(S(y1, z0 + r), S(y1, z1 - r))
    L.addByTwoPoints(S(y1 - r, z1), S(y0 + r, z1))
    L.addByTwoPoints(S(y0, z1 - r), S(y0, z0 + r))
    s45 = math.sin(math.pi / 4)
    A.addByThreePoints(S(y1 - r, z0), S(y1 - r + r * s45, z0 + r - r * s45), S(y1, z0 + r))
    A.addByThreePoints(S(y1, z1 - r), S(y1 - r + r * s45, z1 - r + r * s45), S(y1 - r, z1))
    A.addByThreePoints(S(y0 + r, z1), S(y0 + r - r * s45, z1 - r + r * s45), S(y0, z1 - r))
    A.addByThreePoints(S(y0, z0 + r), S(y0 + r - r * s45, z0 + r - r * s45), S(y0 + r, z0))
    inp = ext.createInput(sk.profiles.item(0), CUT)
    inp.setOneSideExtent(adsk.fusion.DistanceExtentDefinition.create(adsk.core.ValueInput.createByReal(6.0 * MM)),
                         adsk.fusion.ExtentDirections.PositiveExtentDirection)
    inp.participantBodies = [lid, tray]
    ext.add(inp)

    # ---- fillets ----
    do_fillet(vertical_edges(lid, [(TAB_X0, TAB_Y0), (TAB_X0, TAB_Y1)]), 2.5, 'lid tab outer corners')
    do_fillet(vertical_edges(lid, [(TAB_X0 + LID_WALL, TAB_Y0 + LID_WALL), (TAB_X0 + LID_WALL, TAB_Y1 - LID_WALL)]),
              0.9, 'lid tab inner corners')
    do_fillet(vertical_edges(lid, [(CASE_X0, TAB_Y0), (CASE_X0, TAB_Y1)]), 2.0, 'lid tab/case junction')
    do_fillet(vertical_edges(tray, [(TAB_X0, TAB_Y0), (TAB_X0, TAB_Y1)]), 2.5, 'tray far corners')
    do_fillet(vertical_edges(tray, [(TRAY_X1, TAB_Y0), (TRAY_X1, TAB_Y1)]), 1.0, 'tray near corners')
    do_fillet(vertical_edges(tray, [(TAB_X0 + TRAY_WALL, TAB_Y0 + TRAY_WALL), (TAB_X0 + TRAY_WALL, TAB_Y1 - TRAY_WALL)]),
              1.3, 'tray inner far corners')
    do_fillet(outer_loop_edges(planar_face_at(lid, lid_top)), 1.0, 'lid top edge', True)
    bot = planar_face_at(tray, 0.0)
    if not do_fillet(outer_loop_edges(bot), 0.8, 'tray bottom edge', True):
        def in_notch(e):
            bb = e.boundingBox
            return bb.minPoint.x < -17.0 * MM and bb.minPoint.y > (y0 - 0.1) * MM and bb.maxPoint.y < (y1 + 0.1) * MM
        do_fillet(outer_loop_edges(bot, in_notch), 0.8, 'tray bottom edge w/o notch', True)

    for b, n in ((lid, 'Lid'), (tray, 'Tray')):
        bb = b.boundingBox
        say(n, 'faces', b.faces.count, 'vol mm3', round(b.volume * 1000, 1),
            'bb', [round(v * 10, 2) for v in (bb.minPoint.x, bb.minPoint.y, bb.minPoint.z, bb.maxPoint.x, bb.maxPoint.y, bb.maxPoint.z)])

    # ---- interference against the LilyGo case / pins and the daughter board reference ----
    objs = coll([lid, tray])
    for o in root.occurrences:
        if o.component.name == 'Daughter_ref':
            for b in o.bRepBodies:
                if b.name in ('PCB', 'J1_USB_C', 'H1_pins', 'H2_pins'):
                    objs.add(b)
    for o in root.allOccurrences:
        if o.name in ('0-00_56_40:1', '0-00_60_22:1', '0-00_19_1_4:1', '0-00_19_2_4:1', '0-100_27_23:1'):
            for b in o.bRepBodies:
                objs.add(b)
    if objs.count > 2:
        res = design.analyzeInterference(design.createInterferenceInput(objs))
        say('interferences', res.count)
        for r in res:
            say('  ', r.entityOne.name, '<->', r.entityTwo.name, 'vol mm3', round(r.interferenceBody.volume * 1000, 3))

    # ---- export ----
    em = design.exportManager
    for b in (lid, tray):
        fn = os.path.join(OUT, 'uhk_ble_shell_%s.stl' % b.name.lower())
        opt = em.createSTLExportOptions(b, fn)
        opt.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
        opt.isBinaryFormat = True
        em.execute(opt)
        say('stl', fn, os.path.getsize(fn))
    fn = os.path.join(OUT, 'uhk_ble_shell.step')
    em.execute(em.createSTEPExportOptions(fn, comp))
    say('step', fn, os.path.getsize(fn))
    fn = os.path.join(OUT, 'uhk_ble_shell_assembly.f3d')
    em.execute(em.createFusionArchiveExportOptions(fn, root))
    say('f3d', fn, os.path.getsize(fn))
    if ui:
        ui.messageBox('\n'.join(log[-12:]), 'UHK BLE shell rebuilt')
