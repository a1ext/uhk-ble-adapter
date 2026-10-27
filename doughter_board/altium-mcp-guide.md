# altium-mcp: how-to and traps for an LLM agent

Hard-won notes from driving Altium Designer 25 through the `altium-mcp` server
(source: `m:\!hw\altium-mcp`) on a real 4-layer RF board: schematic generation,
footprints, placement, routing fixes, DRC, silkscreen, Gerbers and a JLCPCB order.
Read this before the first tool call. Everything below was learned by getting it
wrong at least once.

---

## 1. Mental model

- The MCP server (Python) talks to Altium through files: it writes a request,
  launches `X2.EXE -RScriptingSystem:RunScript(...)`, and Altium's DelphiScript
  bridge (`Altium_API.pas` and friends) answers in a response file.
- **The MCP client cancels a call after ~60 s.** Keep every script short. Long
  jobs: split into several calls, or run them from PowerShell in the background.
- **One Altium, one script at a time.** Parallel agent sessions share one sandbox
  file; the server serialises them with a cross-process lock.
- `run_altium_script` runs your DelphiScript body inside a generated
  `Sandbox.pas`. Pass `document=<full path>` to bring that document to the front
  first. `SandboxLog('text')` adds a breadcrumb to the result's `steps`; on a crash
  the result tells you the last step reached — log before risky statements.
- A runtime error in a script opens Altium's debugger / an Error box and wedges
  the script engine. The server auto-recovers (closes the box, `EditScript:Stop`,
  probes with the bridge command `close_sandbox_editor`). If a call times out
  instead, look for a modal dialog (section 6).

## 2. Golden rules

1. **Only the declared scratch variables exist** in `run_altium_script`:
   `S1..S3` (strings), `I1..I3` (ints), `B1` (bool), `Obj1..Obj5`, `List1`.
   Any other name (`Obj6`, `List2`, `X`) is a runtime error. Variables are
   untyped enough that `List1` can hold an object (e.g. a replicated primitive).
2. **Read before you write.** Dump the board / sheet to a text file with a script,
   analyse it in Python, generate an ops file, validate the ops in Python, then
   apply them with one generic apply script. Never "edit and look".
3. **Never modify objects while iterating them** (iterator revisits them forever;
   Altium went to 100 % CPU / 12 GB once). Find first, destroy the iterator,
   then modify; or loop "find one, change it, restart" with a cap.
4. **Avoid `PCBServer.PreProcess`/`PostProcess` in scripts that might crash.**
   A crash between them leaves the PCB server "in a command": every later save
   pops *"A command is currently active and save cannot be completed"* in a loop.
   Wrap single-object edits in `PCBM_BeginModify`/`PCBM_EndModify` messages
   instead (pattern below). If you do use PreProcess, keep the body trivial.
5. **Save explicitly and verify the file mtime** after every milestone
   (section 4). Script edits are only in memory until saved.
6. **Coordinates:** board coords are internal units; always convert with
   `CoordToMMs(x - Board.XOrigin)` / `Board.XOrigin + MMsToCoord(mm)`.
   `FloatToStr` uses the Windows decimal separator (a comma on a Russian locale):
   parse dumps with `float(s.replace(",", "."))`, and when reading ops files in
   Pascal do `StringReplace(line, '.', DecimalSeparator, REPLACEALL)` before
   `StrToFloat`.
7. **Non-ASCII in Pascal literals:** Altium reads `.pas` as ANSI and ignores a
   BOM — use `#NNN` codes.
8. After the user reinstalls the extension, the old server process may keep
   running in an open session; ask them to toggle/restart the extension.

## 3. DelphiScript traps

- A function defined *later in the same unit* is invisible ("Undeclared
  identifier"). Cross-unit calls work in any order.
- Once the debugger has opened a unit in the script editor, `RunScript` runs the
  **editor buffer**, not the file on disk (phantom old code). Close the editor
  copy (`close_sandbox_editor`) before re-running.
- `Client.GetDocumentByPath` returns nil for a document that is not open — use
  `Client.OpenDocument('PCB'|'SCH', path)` then `Client.ShowDocument(doc)`.
- `IPCB_ServerInterface.LoadCompFromLibrary(name, libPath)` returns a
  **library footprint**, not a board component (no `Rotation`, no `x`).
- Calls that crashed (runtime error or access violation in ScriptingSystem.DLL):
  - `Pad.SolderMaskExpansion := x` (setter); use a rule instead.
  - `Rule.Priority := 1` immediately after `Board.AddPCBObject(rule)`.
    New rules already land at priority 1; set priorities in a separate
    iterator pass if needed.
  - `IPCB_ObjectClass.RemoveMemberByName` (does not exist) — use
    `RemoveMember(obj)` / `AddMember(obj)` / `IsMember(obj)`.
  - `ISch_Lib.GetSchComponentByLibRef` (does not exist).
  - Assigning `DatafileLink(0).LibIdentifierKind` directly.
  - `ObjectIDString` on schematic objects.
- `Via.IsTenting_Top := True` is reset to the rule value on `EndModify` and the
  rule's tenting setter is ignored. Tent vias with a **SolderMaskExpansion rule
  scoped `IsVia` with Expansion −0.35 mm** (larger than the via radius).
- **Layer names are spelled the way `String2Layer` reads them**: `'Top Layer'`,
  `'Multi Layer'`, `'Top Overlay'`, `'Mechanical Layer 13'`. `'TopLayer'` or
  `'Mechanical 13'` silently return `eNoLayer` (0) — no error. This also applies
  to the layer field of `create_footprints_batch` specs. Primitives on No Layer
  look fine in a footprint inventory, but once placed on a board the board
  iterator, connectivity and DRC never see them. Check with `Layer2String`.
- **PcbLib current footprint:** `Lib.SetState_CurrentComponent(x)` from a script
  wiped all primitives (except 3D bodies) of the footprint that was current
  before the switch — three footprints were lost that way. Never switch the
  current footprint by script; edit other footprints through
  `GetComponentByName` and save.
- Group iterators of library footprints return **nothing** once
  `AddFilter_ObjectSet` / `AddFilter_LayerSet` are applied; iterate unfiltered
  and test `ObjectId` yourself.
- `LoadCompFromLibrary` reads the library as loaded in Altium; if a footprint
  shows only its body there, the library itself is broken (see the two traps above).
- Board outline: `Board.BoardOutline.SetOutlineContour` crashes. Set
  `PointCount` and assign each vertex: `Obj5 := TPolySegment; Obj5.Kind :=
  ePolySegmentLine; Obj5.vx := ...; Obj5.vy := ...; Outline.Segments[i] := Obj5`
  (same for a new polygon pour before `Board.AddPCBObject`).
  `Board.XOrigin/YOrigin` are writable.
- Rule property names that are not `Gap`: silk-to-mask `SilkToMaskGap`,
  silk-to-silk `SilkToSilkClearance`, mask sliver `MinSolderMaskSliver`,
  polygon connect `ConnectStyle := eDirectConnectToPlane`. Unknown names: list
  `SetState_*` strings in `C:\Program Files\Altium\AD25\System\Advpcb.dll`
  (`strings Advpcb.dll | grep SetState_`).
- A text's `BoundingRectangle` keeps the old size after `Size`/`Width` changes
  until the document is saved, closed and reopened.
- `RunProcess('PCB:ChangeView')` with `ViewName=3D` flipped the view to
  "View from bottom" instead of switching to 3D; Ctrl+F flips it back.
- A sandbox `TStringList` with `Delimiter := '|'; StrictDelimiter := True;
  DelimitedText := line` is the easy field splitter for ops files; another
  `TStringList` fits in an `Obj` variable when `List1` is taken.

## 4. Saving and opening

```pascal
{ save the document that is in front (works for PcbDoc, SchDoc, PcbLib, SchLib) }
Obj1 := Client.GetDocumentByPath('M:\...\Board.PcbDoc');
Client.ShowDocument(Obj1);
ResetParameters;
AddStringParameter('ObjectKind', 'Document');
AddStringParameter('SaveMode', 'Standard');
RunProcess('WorkspaceManager:SaveObject');
```

- Then check the file mtime (`ls -la --time-style=+%H:%M:%S file`). If it did not
  change, the save did not happen.
- `ServerDocument.DoFileSave('')` silently does not save a PcbDoc after script
  edits; it works for a project file: `Client.GetDocumentByPath(prj).DoFileSave('')`.
- New sheet from `build_schematic`: `ServerDoc.SetFileName(path); DoFileSave('')`,
  and remove the stray `SheetN.SchDoc` entry it may leave in the project.
- After an Altium restart a PcbDoc may open as a *Free Document*: open the
  project first with `RunProcess('WorkspaceManager:OpenObject')`
  (`ObjectKind=Project`, `FileName=...`).
- **Close + reopen** (`Client.CloseDocument(doc)`, `Client.OpenDocument`) is the
  cure for stale state: compiled netlist after deleting wires, pads that ignore a
  new mask/paste rule, connectivity of pads added by script.

## 5. Schematic workflow

- **Generate, don't draw.** Keep the circuit in a Python generator (parts, nets,
  coordinates) that writes `symbols.txt` (for `create_symbols_batch`),
  `footprints.txt` (for `create_footprints_batch`) and one JSON per sheet for
  `build_schematic(spec_file=...)`. Give the generator a geometric checker
  (netlist from wires vs intended nets, overlaps of texts/ports/bodies) and
  preview PNGs.
- `build_schematic` resolves pin references (`"U1.13"`) to true pin ends; read
  `dev/SCHEMATIC_CONVENTIONS.md` in the altium-mcp repo before using it.
- **Incremental edits keep component unique IDs** (so the PCB stays linked):
  diff old vs new sheet JSON; `RemoveSchObject` the obsolete wires/ports
  (match wires by their vertex list); move moved parts with `Comp.MoveByXY`;
  append only new objects with `build_schematic(target=<sheet path>)`.
  Then **close and reopen the sheet before compiling**, or `DM_Compile` keeps the
  deleted objects (phantom shorts).
- Footprint links created by `build_schematic` point at "Any" library and the ECO
  then reports *Footprint Not Found*. Fix per implementation:
  `ClearAllDatafileLinks; AddDataFileLink(ModelName, fullPcbLibPath, 'PCBLib');
  UseComponentLibrary := False`.
- Text positions: `Designator.Autoposition := False; Orientation := 0;
  Justification := 0; MoveToXY(...)`; hide a comment with `Comment.IsHidden`.
- Verify connectivity with a compiled netlist dump and compare to the
  generator's expected netlist (pin sets, not net names — auto names change,
  e.g. `NetJ11_1` becomes `NetD11_1` when a part with a lower designator joins):

```pascal
Obj1 := GetWorkspace; Obj2 := nil;
for I1 := 0 to Obj1.DM_ProjectCount - 1 do
  if (Pos('MyBoard.PrjPcb', Obj1.DM_Projects(I1).DM_ProjectFullPath) > 0) then Obj2 := Obj1.DM_Projects(I1);
Obj2.DM_Compile;
List1 := TStringList.Create;
for I1 := 0 to Obj2.DM_LogicalDocumentCount - 1 do
begin
  Obj3 := Obj2.DM_LogicalDocuments(I1);
  for I2 := 0 to Obj3.DM_NetCount - 1 do
  begin
    Obj4 := Obj3.DM_Nets(I2); S1 := 'NET|' + Obj4.DM_NetName;
    for I3 := 0 to Obj4.DM_PinCount - 1 do
      S1 := S1 + '|' + Obj4.DM_Pins(I3).DM_LogicalPartDesignator + '.' + Obj4.DM_Pins(I3).DM_PinNumber;
    List1.Add(S1);
  end;
end;
List1.SaveToFile('C:\Users\Public\altium_mcp\netlist_raw.txt');
```

## 6. Dialogs, ECO and recovery

- **Modal dialogs block every script call** (calls time out). Typical culprits:
  the DRC dialog, the ECO (Engineering Change Order) dialog, Altium's
  "Error" exception box, the save Warning. Inspect Altium's top-level windows from
  PowerShell (EnumWindows + GetWindowThreadProcessId on the X2 process), read
  child control texts, and close only what you understand. Capture a single
  window with `PrintWindow` — never the user's whole screen.
- Altium's own exception box ("Access violation ... ScriptingSystem.DLL", button
  "Send and Close"): posting `WM_CLOSE` to it closed it and Altium kept running.
- **"A command is currently active and save cannot be completed"**: answering
  *Yes* opens "Save a recovery copy as"; save `Copy of X.PcbDoc` (set the file name
  edit, press Save), then the user kills Altium and you rename the copy over the
  original. Untested shortcut to try first: a script containing only
  `PCBServer.PostProcess;`.
- **ECO (Update PCB):** there is no MCP tool, and scripts always see *Free
  Documents* as the focused project, so `WorkspaceManager:Compare` from a script
  fails. The user must click the project in the Projects panel once; then
  `X2.EXE -RWorkspaceManager:Compare(ObjectKind=Project|Action=UpdateMe)` from the
  command line opens the ECO. Prefer **adding components by script** (section 7)
  when only a few parts are new.
- `X2.EXE -R...` command lines only act when Altium is in the foreground and idle;
  call the MCP `get_screenshot` tool first (it brings Altium to the front).

## 7. PCB recipes

### Dump the board (analyse in Python)

```pascal
Obj1 := PCBServer.GetCurrentPCBBoard;
List1 := TStringList.Create;
Obj3 := Obj1.BoardIterator_Create;
Obj3.AddFilter_ObjectSet(MkSet(eTrackObject, eViaObject, ePadObject));
Obj3.AddFilter_LayerSet(AllLayers);
Obj3.AddFilter_Method(eProcessAll);
Obj4 := Obj3.FirstPCBObject;
while (Obj4 <> nil) do
begin
  S1 := ''; if (Obj4.Net <> nil) then S1 := Obj4.Net.Name;
  if (Obj4.ObjectId = eTrackObject) then
    List1.Add('T|' + Layer2String(Obj4.Layer) + '|' + S1 + '|' + FloatToStr(CoordToMMs(Obj4.x1 - Obj1.XOrigin)) + '|' + FloatToStr(CoordToMMs(Obj4.y1 - Obj1.YOrigin)) + '|' + FloatToStr(CoordToMMs(Obj4.x2 - Obj1.XOrigin)) + '|' + FloatToStr(CoordToMMs(Obj4.y2 - Obj1.YOrigin)) + '|' + FloatToStr(CoordToMMs(Obj4.Width)))
  else if (Obj4.ObjectId = eViaObject) then
    List1.Add('V|' + S1 + '|' + FloatToStr(CoordToMMs(Obj4.x - Obj1.XOrigin)) + '|' + FloatToStr(CoordToMMs(Obj4.y - Obj1.YOrigin)) + '|' + FloatToStr(CoordToMMs(Obj4.Size)) + '|' + FloatToStr(CoordToMMs(Obj4.HoleSize)))
  else
  begin
    S2 := ''; if Obj4.InComponent then S2 := Obj4.Component.Name.Text + '.' + Obj4.Name;
    List1.Add('P|' + Layer2String(Obj4.Layer) + '|' + S1 + '|' + FloatToStr(CoordToMMs(Obj4.BoundingRectangle.Left - Obj1.XOrigin)) + '|' + FloatToStr(CoordToMMs(Obj4.BoundingRectangle.Bottom - Obj1.YOrigin)) + '|' + FloatToStr(CoordToMMs(Obj4.BoundingRectangle.Right - Obj1.XOrigin)) + '|' + FloatToStr(CoordToMMs(Obj4.BoundingRectangle.Top - Obj1.YOrigin)) + '|' + S2);
  end;
  Obj4 := Obj3.NextPCBObject;
end;
Obj1.BoardIterator_Destroy(Obj3);
List1.SaveToFile('C:\Users\Public\altium_mcp\board_dump.txt');
ResultText := IntToStr(List1.Count); List1.Free;
```

- The track list includes **Mechanical and Overlay lines** (courtyards, 3D
  outlines, silk). Filter to copper layers before clearance math — a via search
  once found "no room" anywhere because courtyard lines counted as copper.
- Pad bounding boxes include nothing extra, but remember mask/paste expansion
  when checking manufacturing rules.

### Apply edits through an ops file

Write ops in Python (`VIA|x|y|nx|ny[|size|hole]`, `END|x1|y1|x2|y2|end|nx|ny`,
`NEW|x1|y1|x2|y2|w|net[|B]`, `DELV|x|y`, `DELT|x1|y1|x2|y2`), validate them in
Python against the dump (clearances per layer, via-to-pad gap, hole spacing,
track ends landing on same-net copper), then apply with one generic script:
find the object by coordinates (±0.003 mm), and modify it inside

```pascal
PCBServer.SendMessageToRobots(Obj2.I_ObjectAddress, c_Broadcast, PCBM_BeginModify, c_NoEventData);
Obj2.x := Obj1.XOrigin + MMsToCoord(...);   { or x1/y1/x2/y2, Size, HoleSize }
PCBServer.SendMessageToRobots(Obj2.I_ObjectAddress, c_Broadcast, PCBM_EndModify, c_NoEventData);
```

Delete with `Board.RemovePCBObject(obj)`; a component primitive needs
`Comp.RemovePCBObject(prim); Board.RemovePCBObject(prim)`. New track: factory
`eTrackObject`, set coords/width/layer, `Net := netObj`, `Board.AddPCBObject`.
Finish with a **repour**:

```pascal
Obj3 := Obj1.BoardIterator_Create;
Obj3.AddFilter_ObjectSet(MkSet(ePolyObject)); Obj3.AddFilter_LayerSet(AllLayers); Obj3.AddFilter_Method(eProcessAll);
Obj4 := Obj3.FirstPCBObject;
while (Obj4 <> nil) do begin Obj4.SetState_CopperPourInvalid; Obj4.Rebuild; Obj4.CopperPourValidate; Obj4 := Obj3.NextPCBObject; end;
Obj1.BoardIterator_Destroy(Obj3);
```

### Add a component without an ECO

1. Footprint into the PcbLib with `create_footprints_batch`, save the PcbLib.
2. Per component (one script call each):
   `Lib := PCBServer.LoadCompFromLibrary(name, libPath)`;
   `Comp := PCBServer.PCBObjectFactory(eComponentObject, eNoDimension, eCreate_Default)`;
   set `Comp.x/y`, `Pattern`, `Layer`; `Board.AddPCBObject(Comp)`; for every
   primitive of `Lib` (group iterator): `P := prim.Replicate; P.MoveByXY(Comp.x, Comp.y);
   Board.AddPCBObject(P); Comp.AddPCBObject(P)`. Then `Name.Text`,
   `Comment.Text`, `Rotation` (rotates the children), `NameOn/CommentOn`, and the
   schematic link: `SourceUniqueId := '\' + <schematic UniqueId>`,
   `SourceDesignator`, `SourceLibReference`, `SourceFootprintLibrary`.
3. **Pad nets**, without PreProcess:
   `BeginModify; Pad.Net := net; EndModify; net.AddPCBObject(pad)`.
   Without `AddPCBObject` the pad is not in the net: DRC reports "Short-Circuit"
   against its own net and the net is **lost on save**.
4. Save, close, reopen, repour, DRC.
5. Verify: count pads per component (group iterator) **and** pads seen by a
   board iterator; they must match. A mismatch meant pads on No Layer. Compare
   the dumped pad centres with the Python model (bounding boxes include the mask
   expansion on every side).

Footprints converted from KiCad: the `.kicad_mod` → spec conversion works well
(mm → mils, negate Y, F.SilkS/F.Fab/F.CrtYd → Top Overlay / Mechanical 13 / 15),
STEP via `ComponentBody.ModelFactory_FromFilename(path, True)`. KiCad tantalum
and 1206 silk lines run long; in a dense column they violate silk-to-mask
against the neighbours' pads — delete them on the board and add a short
polarity bar where it stays visible.

### Rules, nets, classes

- Create rules with `PCBServer.PCBRuleFactory(eRule_...)`, set `Name`,
  `Scope1Expression` (`'HasFootprint(''X'')'`, `'InComponent(''U1'')'`,
  `'InNetClass(''RF_50'')'`, `'IsVia'`), the value (`Gap`, `Expansion`), add to
  the board. Mask/paste rules with a negative expansion bigger than half the pad
  remove the opening (DNP parts: no paste so bare pads cannot bridge).
- **Mask and paste expansion are cached per pad**: after a new rule, close and
  reopen the PcbDoc, otherwise DRC and Gerbers still use the old value.
- Rename a net: `cls.RemoveMember(net); net.Name := new; cls.AddMember(net)`
  for every class it belongs to (classes store names).
- `place_components` does not refresh the ratsnest/connectivity: save, close,
  reopen, then `Board.AnalyzeNet(net)` per net.

### DRC from outside (PowerShell)

`X2.EXE "-RPCB:DesignRuleCheck"` opens the DRC dialog only with Altium in
front (`get_screenshot` first). `BM_CLICK` on the Run button is unreliable;
posting mouse messages works. The `.drc` text report in
`Project Outputs for <project>\` is rewritten each run; check its `Time` line,
or count violations with a script (more reliable):

```powershell
# W = small C# helper: EnumWindows, GetWindowThreadProcessId, GetWindowText,
#     GetClassName, EnumChildWindows, PostMessage (Add-Type)
$x2 = @(Get-Process -Name X2 | % { $_.Id })
Start-Process "C:\Program Files\Altium\AD25\X2.EXE" -ArgumentList '"-RPCB:DesignRuleCheck"'
# wait for a top window of $x2 titled "*Design Rule Checker*", sleep 3 s, then
# on its child whose text (minus '&') is "Run Design Rule Check*":
[W]::PostMessage($btn, 0x0201, [IntPtr]1, [IntPtr]0x000A000A)   # WM_LBUTTONDOWN
[W]::PostMessage($btn, 0x0202, [IntPtr]0, [IntPtr]0x000A000A)   # WM_LBUTTONUP
```

```pascal
{ count and describe violations }
Obj3 := Obj1.BoardIterator_Create;
Obj3.AddFilter_ObjectSet(MkSet(eViolationObject)); Obj3.AddFilter_LayerSet(AllLayers); Obj3.AddFilter_Method(eProcessAll);
Obj4 := Obj3.FirstPCBObject; I1 := 0; S1 := '';
while (Obj4 <> nil) do
begin
  I1 := I1 + 1;
  S1 := S1 + Obj4.Rule.Name;
  if (Obj4.Primitive1 <> nil) then S1 := S1 + ' ' + Obj4.Primitive1.Descriptor;
  if (Obj4.Primitive2 <> nil) then S1 := S1 + ' / ' + Obj4.Primitive2.Descriptor;
  S1 := S1 + ' || ';
  Obj4 := Obj3.NextPCBObject;
end;
Obj1.BoardIterator_Destroy(Obj3);
ResultText := IntToStr(I1) + ' violations ' + S1;
```

A DRC dialog left open is modal — every following script call times out.
The batch DRC does not run the Component Clearance rule by default (it is not
in the `.drc` report) — courtyard/body overlaps need their own check.
A complete helper script: `doughter_board/run_drc.ps1` in uhk-ble-adapter.

### Silkscreen

- Altium silk-to-silk default is 10 mil (0.254 mm); silk-to-mask 0.05 mm. JLC
  flags silk < 0.13 mm from a pad or a hole.
- A designator's `BoundingRectangle` is the glyph box plus ~0.1 mm; for a
  rotated text it is the rotated box. When reading dumped boxes back, un-rotate
  them by the text rotation before reusing the size.
- Place a designator exactly: `Comp.ChangeNameAutoposition := eAutoPos_Manual;
  Name.Rotation := r; Name.MoveByXY(target - BoundingRectangle)` (Begin/EndModify
  on the text).
- Newly placed components get Altium's default (huge) designator size — copy
  `TextKind/Size/Width` from an existing designator.
- Free texts (board labels) are obstacles too; check them, not only designators.

## 8. Outputs and manufacturing

- The OutJob must be **part of the project**, or its relative document path stays
  unresolved and generation pops an Error box. `run_output_jobs` needs
  `container_names` (get them from `get_output_job_containers`).
- Check the generated Gerbers yourself (silk vs holes and mask openings, via
  holes vs SMD pads) with a small Python Gerber/Excellon parser before uploading
  to a fab DFM: Altium's DRC measures text boxes and via rings, the fab measures
  plotted strokes and drill holes. Gotchas: Altium writes flashes as
  `X..Y..D02*` + `D03*`, uses modal coordinates (in Excellon too — a hole with an
  unchanged Y has no `Y`), polygon pours are `G36/G37` regions without an
  aperture, via rings are flashes centred on the drill hits.
- **JLCPCB assembly (CPL):** JLC places its own EasyEDA footprint (by LCSC code)
  at the CPL point and rotation, with its own 0° orientation and origin. Fetch
  `https://easyeda.com/api/products/<LCSC>/components?version=6.4.19.5`
  (from an easyeda.com browser tab): `result.packageDetail.dataStr.head.x/y` is
  the origin; `PAD~shape~x~y~w~h~layer~net~number...` in 10-mil units, y pointing
  down. Fit rotation and offset per part so JLC's pads land on yours, and apply
  the corrections in the CPL generator. Seen on one board: TSSOP/QFN/SOT-23
  −90°, SC-70 +180°, Hirose U.FL origin 0.625 mm toward the signal pad;
  passives, 3225 crystal and an 0201 diode matched.
- DNP parts: leave them out of BOM and CPL, and remove their paste.

## 9. Working style that paid off

- Keep scripts small and idempotent; one purpose per `run_altium_script` call.
- `SandboxLog` before every statement you are not sure about.
- Python does the geometry (searching spots, clearance checks, placement of
  texts); Pascal only dumps and applies.
- After each batch: repour → DRC → save → check mtime.
- Back up before risky operations (copy the PcbDoc); never overwrite the user's
  files without checking what is there.
- Report what you verified (DRC count, Gerber check result, file times) and what
  you did not.
