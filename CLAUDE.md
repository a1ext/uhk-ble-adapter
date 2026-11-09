# UHK 60 v2 → BLE adapter (DIY)

## Goal
Homemade USB→Bluetooth adapter for an Ultimate Hacking Keyboard 60 v2: the keyboard plugs
into the board's USB-C, the ESP32-S3 is its USB host and a BLE HID device for up to 5
hosts (UHK Agent works over BLE too). Power comes from a 5 V supply on the board's VBUS
pin (USB port or charger).
- 2026-10-09: the analog USB mux and the firmware's Cable mode were dropped. For cable use
  the keyboard is plugged straight into the PC.

## Hardware decisions
- MCU board: **LilyGO T-Display S3 AMOLED Plus** (ESP32-S3R8, single USB-C = native USB).
  Schematic: `../../Lilygo/LilyGo-AMOLED-Series/schematic/T-Display-S3-AMOLED-Plus.pdf`.
  - Its USB-C is the **USB host port** for the keyboard: the keyboard's cable directly, or
    a panel USB-C jack wired to a USB-C plug breakout ("pigtail"; D+/D-/VBUS/GND straight,
    56k Rp from CC1/CC2 to VBUS). GPIO19/20 are not on the headers.
  - USB-C VBUS and the header **VBUS pins are the same net** (only a TVS): +5 V fed into
    the header VBUS powers the board AND the keyboard (the board itself does not source
    VBUS, BQ25896 OTG is off). Never have the board's USB-C in a PC while 5V is fed into the
    header VBUS. CC1/CC2 have 5.1k Rd on board.
  - PMU is BQ25896 (PSEL variant, does not touch D+/D-); the LilyGo lib disables charging/OTG.
  - UART0 (GPIO43 TX / GPIO44 RX, CN1) only carries the debug log; it is not part of the
    adapter. Flashing = BOOT held at power-up / Reset → ROM download over USB, or BLE OTA.
- GPIO assignment:
  - The pairing button is the board's **BOOT** (GPIO0), at the bottom of the case: hold
    1.5 s = pairing mode for the active slot, press while pairing = cancel, a short press
    otherwise = settings menu. (BOOT held
    during power-up / reset still enters the ROM bootloader.) No extra buttons.
  - Spare: GPIO10, GPIO16, GPIO39–42 (header).
  - Taken / avoid: 0, 3, 45, 46 strapping; 1 PMU IRQ; 2/3 I2C (touch, RTC, PMU); 4 battery
    divider; 5, 6, 7, 9, 17, 18, 38, 47, 48 display; 11–14 SD card (11 also touch, per user);
    15 RTC INT; 21 touch INT; 19/20 USB; 43/44 UART0.
- Power input: USB 2.0 USB-C receptacle (5.1k Rd on CC1 and CC2 for C-to-C cables, D+/D-
  unconnected) → polyfuse ~1 A → header VBUS; 100–220 µF bulk cap near the board.
- Wiring / pinout pictures for users: `docs/img/power-board.svg` (power board circuit +
  back-view placement), `docs/img/pinout.svg`, `docs/img/wiring.svg` (hand-wired),
  `docs/img/shell.jpg` (shell render); shown on the Pages site and in the README.
- Power daughter board (`doughter_board/`, Altium; project file currently under
  `M:\bt-rf-backend\docs\hardware\DaughterBoard\`): USB-C sink (5.1k Rd, D± 15k to GND), PTC
  1.1 A, 47µ/10µ/1µ/100n, male 1×4 pins onto LilyGo pins 11–14 of both rows. Layout source of
  truth: `pcb_layout.py` (+ `silk_check.py`); LilyGo frame: DXF/STEP have USB-C at y = 0, pins
  11–14 at y 23.54…15.92 (15.9–23.5 mm from the USB-C edge). The board lies component side on
  the case back (sockets flush, ~2.3 mm of pin, header plastic removed); everything except the
  pins sits on a tab beyond the VBUS-side case wall, USB-C facing away from the LilyGo.
  3D-printed shell (Fusion): `doughter_board/shell/` (Lid + Tray, M1.2 screws, PRINT.md).

## Keyboard facts (USBTreeView + UHK firmware sources, right/src/hid/*_app.hpp)
- VID:PID **0x37A8:0x0003** ("Ultimate Gadget Laboratories", "UHK 60 v2")
  (older firmware used 1D50:6122 — don't filter on VID/PID).
- USB 2.0, **Full-Speed only (12 Mbit/s)**, bMaxPower 500 mA; ESP+AMOLED adds ~100–250 mA →
  prefer USB 3 port (900 mA) or charger.
- **4 HID interfaces**, one interrupt IN endpoint each (+ EP0), no OUT endpoints:

  | IF | EP / interval | Report | IDs |
  |---|---|---|---|
  | 0 | 0x81, 1 ms, 29 B | keyboard (boot-capable), NKRO: modifiers + bitfield; LED output | none |
  | 1 | 0x82, 1 ms, 12 B | mouse: 24 buttons, X/Y int16, hi-res wheel + pan (resolution multiplier feature) | 1 |
  | 2 | 0x83, 8 ms, 63 B | UHK Agent command channel, vendor page 0xFF00 usage 1; host→kbd via SET_REPORT(Output) | none |
  | 3 | 0x84, 1 ms, 10 B | consumer (4 × u16 array) + system (2 × u8 array) | none |
- Switching NKRO ↔ 6KRO in Agent changes the keyboard report descriptor (re-enumeration).

## Firmware updates
- Fresh board: web installer on GitHub Pages (`docs/index.html` + `docs/manifest.json`,
  ESP Web Tools 10.4.0, Web Serial: Chrome / Edge / Opera desktop). `tools/pio_extra.py`
  merges bootloader + partitions + boot_app0 + firmware into
  `$BUILD_DIR/uhk-ble-adapter.bin` (flash at 0x0, same flash params as upload: DIO, 80 MHz,
  16 MB) after every build. CI (`.github/workflows/build.yml`): builds on push / PR,
  deploys `docs/` + the merged image to Pages on push to master and on tags (manifest
  version = `git describe`), tags `v*` → GitHub release with `-full.bin` and `-ota.bin` (+ the Windows tool .exe files,
  see `src/live.*`).
  Pages must be enabled once: Settings → Pages → Source: GitHub Actions.
- USB: `pio run -t upload` — the running firmware owns the USB port (host), so enter
  the ROM bootloader first (BOOT + RST); disconnect the 5 V supply and the keyboard.
- **Bluetooth (Windows): `pio run -t ota`** → `tools/ble_ota.py` uploads `firmware.bin` to the
  paired adapter through Windows' own GATT connection (no advertising needed). Needs a
  Python with the WinRT projections (`pip install bleak`); `UHK_OTA_PYTHON` selects the
  interpreter. `src/ota.*`: custom GATT service (MITM-encrypted link of the active slot's
  authenticated host only, `ble_hid_ready()` checked on every write), the image goes
  straight into the inactive app partition (A/B, default_16MB.csv app0/app1) while the
  firmware keeps running; SHA-256 + esp_ota_end validation, then boot partition switch and
  reboot. Rollback: `verifyRollbackLater()` returns true, the new
  image stays pending-verify until a BLE host has been connected 3 s (or 5 min uptime);
  a crash / reset before that → the bootloader boots the previous image.

## Firmware (PlatformIO, `pio run -t upload`)
- Upload uses pioarduino's esptool 4.8.9 with `--after watchdog_reset`: over USB-Serial-JTAG
  the default RTS hard reset leaves the chip in the ROM bootloader (black screen).
- espressif32@6.7.0 = Arduino core 2.0.16 / ESP-IDF 4.4.7; board JSON and vendored
  `lib/LilyGo-AMOLED-Series` copied from `../../Lilygo/ble-mouse`; lvgl 8.4.0 /
  SensorLib 0.2.4 / XPowersLib 0.2.7 from the PlatformIO registry (`lib_deps`, pinned to
  what the LilyGo repo bundles) — the build is self-contained (CI builds it).
- `Serial` = UART0 (CDC_ON_BOOT=0); logs are mirrored to native USB CDC until the USB host
  takes the PHY, and the log tail is shown on the screen.
- `src/usb_hid_host.*` — IDF USB Host Library: claims all HID interfaces, reads report
  descriptors, polls IN endpoints, SET_REPORT (LEDs / feature) on behalf of the BLE host.
  Installed at boot (`bridge_start()` after the first UI frame): from then on the board's
  USB port is a host, no USB CDC / JTAG — flashing by USB needs the ROM bootloader.
- `src/hid_desc.*` — report descriptor parser / rewriter. The Report ID added to an
  interface without IDs goes right after its top-level `Collection (Application)`: Windows
  (hidbthle) fails the device with code 10 "Report ID declared outside a top-level
  collection" if it precedes the collection.
- `BLEHIDDevice::pnp()` stores VID/PID/version big-endian while the PnP ID characteristic is
  little-endian — pass them byte-swapped (Windows showed VID A837 PID 0300).
- `src/bridge.*` — BLE report map = keyboard's own descriptors concatenated with renumbered
  report IDs, cached in NVS with the keyboard VID:PID (BLE starts at boot before USB
  enumerates). Descriptors / VID:PID changed → forget pairings + restart (hosts cache the
  map); cache format change (`BLOB_VERSION`) → old cache dropped, pairings forgotten. A USB
  device without a keyboard collection (UHK bootloader) is ignored, the map is kept.
  - UHK Agent over BLE (`BLE_AGENT_CHANNEL`): the vendor command channel (IF2) is the only
    vendor interface forwarded, in a **second HID service** with its descriptor as is (no
    report IDs) → on the host a separate HID device, like the USB interface; Agent needs no
    flags. (First attempt merged it into the main map with a report ID + `--report-id`:
    did not work on macOS — a HID device that contains a keyboard needs Input Monitoring
    permission there and mixes keyboard reports into Agent's reads.) The main HID service
    is created first, Android uses only one HID service. The BLE PnP ID is the keyboard's
    USB VID:PID (37A8:0003), so Agent matches it as UHK 60 v2. Agent output reports are
    forwarded as SET_REPORT and never cached / replayed. The second 0x1812 service MUST be
    created with `inst_id` 1: the Arduino BLE lib matches ESP_GATTS_CREATE_EVT by UUID +
    inst_id, with 0/0 it overwrote the main HID service's handle and scrambled its
    characteristic / descriptor handles (reads unanswered → Windows "GATT unreachable", no
    services; Mac / iOS paired but got no input). The official Agent does not talk to
    UHK 80 over BLE either (BLE PIDs ≥ 0x8000 are only flagged as `bleDeviceConnected`).
  - Agent command filter (`agentAccess()` in bridge.cpp, only output reports are commands):
    read-only status commands (0x00, 0x09, 0x0b, 0x0c, 0x0e, 0x0f, 0x12, 0x1b, 0x22, and
    ReadConfig 0x04 of buffer 0 = hardware config) always pass; everything else (user config
    read / write — macros may hold passwords —, ApplyConfig, SetVariable, ExecMacroCommand,
    SwitchKeymap, …) needs "Allow" on the screen: a refused command gets InvalidCommand
    (status 1) and pops up "UHK Agent asks for config access" (Deny / Allow, 30 s). Access
    lasts 10 min after the last such command and ends when the host disconnects; a "Agent
    open N min - tap to lock" button shows it. After "Allow", retry in Agent. Firmware
    commands (0x01 Reenumerate, 0x02/0x03 module bootloader / KBOOT, 0x20/0x21 module flash)
    are always refused — keyboard firmware updates with the keyboard on the PC by cable.
  FIFO: keyboard reports never merged (macros), mouse reports with equal buttons merged by
  summing relative axes; USB polling paused when the FIFO is high (backpressure).
- `src/ble_hid.*` — Bluedroid BLEHIDDevice, MTU 185 (NKRO needs ≥ 32), at most 2
  notifications queued in the controller.
  - Security: LE Secure Connections + passkey entry — IO capability DisplayOnly, the
    6-digit code pops up on the screen, the host types it (`SC_MITM_BOND`,
    `ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH`: Just Works / legacy pairing refused; in
    Bluedroid that check compares the *selected association model*, not the peer's AuthReq
    bits). All HID / Agent / OTA attributes need `*_ENC_MITM`. Bonds of older firmware
    (Just Works) were dropped with `BLOB_VERSION` 5.
  - Reports flow only once `authorized` (`ble_hid_ready()`): after encryption the loop
    checks who it is (`onSecured()`): a pairing in pairing mode (passkey shown / SMP key
    event / new bond in the bond list) records the slot host; a reconnect must resolve to
    the ACTIVE slot's host, otherwise it is dropped — Bluedroid bonds are not tied to our
    (slot) address, any bonded host could connect to any slot address. Identity: bond
    address, PID static address, or RPA resolved with the bond's IRK (`ah()` via mbedtls
    AES, Core spec test vector checked; Bluedroid stores the IRK LSB first — both orders are
    tried). CCCDs are forced on at that point (the Arduino lib does not persist them), host
    writes before it are ignored. A link not authorized within 10 s (60 s while pairing,
    passkey typing) is dropped. Bonds no slot refers to are removed at start and after
    each pairing.
  - Bluedroid clears a bonded host's keys in RAM as soon as a pairing request arrives,
    before asking us (`smp_proc_pair_cmd`): a refused pairing attempt that spoofs a host's
    public address breaks that host until the adapter restarts (DoS only).
  - 5 host slots (`BLE_SLOTS`), each a separate BLE identity: static random address (chip MAC + slot +
    generation) and name "UHK60 BLE n"; our identity key is not distributed, so hosts bind
    to the slot address. Slot = recorded host address (NVS "bleslots").
  - Advertising is built raw (flags, appearance keyboard, complete 16-bit UUID 0x1812, name
    — all in the advertisement, no scan response). With Bluedroid's own builder (UUID in an
    *incomplete* list, name only in the scan response) Windows and iOS did not list the
    adapter while Pixel and macOS did — suspected cause, not confirmed yet.
  - Normally the adapter is NOT discoverable (no LE discoverable flag) and advertises only
    if the slot has a host, so only that host reconnects. Pairing mode (physical button or
    on-screen "Pair", hold 1.5 s; while the physical button is held a pink→blue gradient
    bar along the bottom edge shows how long to keep holding): current host dropped, slot discoverable for 20 s; a used slot advertises its
    next-generation address meanwhile, committed when a new host pairs (old bond removed),
    reverted on cancel / timeout. Pairing requests outside pairing mode are always refused
    (a host that lost its keys pairs again in pairing mode).
  - The slot host is recorded ONLY when pairing completes in pairing mode, as the identity
    address of the new bond (bond list diff). Auth-complete also fires on every reconnect,
    and phones / Macs reconnect with a fresh private address — treating that as a new host
    (an earlier bug) deleted the host's bond and rotated the slot address. The pairing
    window does not close under a connected host that is still pairing (Windows reads GATT
    first).
- A crash / WDT / brownout reset only shows a note on the screen (there is no safe mode;
  recovery = ROM bootloader, a bad OTA image rolls back by itself).
- `src/stats.*` — typing statistics: the bridge counts every keyboard USB input report
  (also without a BLE host): `hid_parse()` marks key / button fields (bitmaps on the
  keyboard / button pages, arrays on keyboard / consumer / generic desktop) and the X/Y
  elements of relative fields; a key / click = a bit going 0 → 1 or an array code that was
  not in the previous report (codes 1–3 = rollover / errors ignored); travel = √(dx²+dy²)
  in counts, shown as metres at `MOUSE_CPI` (800). Totals in NVS ("stats": keys, clicks,
  travel), written every `STATS_SAVE_MS` (1 h), when the screen goes dark and before OTA /
  descriptor-change restarts — a crash loses at most an hour.
- `src/anim.*` — standby animations: one UANM file (format in anim.h: RGB565 byte-swapped,
  per-frame delta tokens skip / run / literal, scale 1 or 2 = pixel-doubled) in the unused
  "spiffs" data partition (3.4 MB), played straight to the panel (`display_push`) while
  LVGL is paused. Uploaded through the OTA service with begin op 0x03 — no reboot: the
  player stops, the partition is written, SHA-256 + header/index checked, then reloaded.
  OTA control commands 0x10 play / 0x11 stop / 0x12 settings / 0x13 info (NVS "anim":
  mode, delay, brightness, off-after; default standby after 60 s idle, brightness 80, screen
  off after 30 min of animation; mode 0 off / 1 animation / 2 dashboard / 3 both in turns,
  3 min dashboard then 2 min animation, `main.cpp` standby logic). Touch stops it (the
  touch is swallowed); keyboard typing never brings the main screen back. "Keyboard
  wakes the screen" (`kbdWake`, `kbdIdleS`; NVS "kbd" / "kbdidle"; 0x12 / 0x13 carry them
  after the night brightness; `bridge_last_input_ms()` = last key / click / motion): keys
  on a dark screen start the standby screen and hold it, `kbdIdleS` after the last key it
  goes dark (`standbyByKbd` in main.cpp; a standby screen the keyboard never touched keeps
  the off-after rule). `kbdIdleS` 0 = "auto": the keyboard's own backlight timeout, which
  bridge.cpp reads from the keyboard 1.5 s after it comes up (`probe*`: Agent ReadConfig
  0x04 of buffer 2 = validated user config, two 63-byte chunks, replies taken off the Agent
  input stream and not forwarded; `parseBacklightTimeout()` mirrors the UHK firmware's
  parse_config.c header: version u16×3, length u32, device name (compact length), double-tap
  timeout, then `keyBacklightFadeOutTimeout` for data model ≥ 8 — ledsFadeTimeout for 6–7;
  value in seconds, 0 / unreadable → 60 s). Not sent while a host Agent command went out in
  the last 2 s. Exposed as `bridge_keyboard_backlight_timeout_s()`, the info reply (0x13)
  carries it, the menu hint shows it. PC side: `tools/ble_anim.py` (make / preview / upload / play /
  stop / info / set; input = `demo:<name>` from `tools/anim_demos.py`, GIF / WebP / PNG
  folder, or .uanm), encoder `tools/uanm.py`, shared WinRT link `tools/ble_link.py`
  (also used by ble_ota.py).
- `src/live.*` — live audio spectrum: `tools/ble_live.py` captures the PC's output (WASAPI
  loopback via `soundcard`), FFT → 8..64 log bands, sends only levels ("LV" 01 <bright>
  <u8 × n>, ~50 B at 60 fps) on the OTA **data** characteristic while no transfer runs;
  the adapter draws neon bars + reflection + falling peak caps, pushing only the changed
  rows of each bar (the Plus panel is 1-bit SPI at 40 MHz: a full screen takes ~51 ms).
  Starts with the stream, ends 1.5 s after the last packet or on "LV" 00; touch / button /
  pop-up shows the normal screen, the spectrum returns after 30 s (a restarted stream at
  once). Pattern from `M:\!hw\VFD\sketch_aug15a\vfd_spectrum.py`.
  Standalone Windows executables (no Python needed): `python tools/build_exe.py [live]
  [anim]` → own `.venv-build` (uv) + PyInstaller `--onefile`:
  - `dist/uhk-ble-live.exe` (~30 MB, `--windowed`) of `tools/live_tray.py` (pystray tray
    icon: status, Pause/Resume, Auto gain, Open log, Exit; reconnects every 10 s; single
    instance via a named mutex; stdout/stderr → `%LOCALAPPDATA%\uhk-ble-live\live.log`;
    `--hidden-import pystray._win32`, `--collect-data soundcard`; icon from
    `tools/live_icon.py`).
  - `dist/uhk-ble-anim.exe` (~28 MB, console) of `tools/ble_anim.py`: a single existing
    path argument (file dropped on it) = `upload`; no arguments = help; waits for Enter
    when it owns its console (`GetConsoleProcessList`).
  - Both use `--collect-submodules winrt`.
  - CI job `tools` (windows-latest) builds both; the Pages deploy puts them in
    `download/` next to the web installer (`docs/index.html` "Animation and live spectrum
    (Windows)" links them), and tags attach them to the release.
  Exit / Pause cancel the stream: stop packet + GATT service released. A killed tool
  otherwise keeps the service busy ~20 s (ble_link waits up to ~24 s); the console
  `ble_live.py` handles closing its window (SetConsoleCtrlHandler). **One tool at a
  time:** while the live stream holds the service, the others get ACCESS_DENIED
  (ble_link then says "the adapter is busy"). It must not be shared anyway: during an
  upload, live packets on DATA would go into the image.
- Brightness of the standby animation / live spectrum follows the time of day
  (`anim_brightness_now()`): night level (default 30) until 6:00, up to the day level
  (default 180) by 9:00, down again 18:00–22:00, smoothstep in perceived brightness. Clock
  = the board's PCF85063 RTC (I2C 0x51), backed by a rechargeable MS412FE cell (U8, charged
  from 3V3 via D9 — it keeps time without power); `ble_link.py` sets local time (OTA control
  command 0x14) on every connection. RTC OS flag set = not valid → day level.
- `src/sysmon.*` + the dashboard in `ui.cpp`: "SY" packets on the DATA characteristic
  (17 B, 2/s: cpu / ram / gpu / vram %, net rx / tx KB/s, disk r / w in 100 KB/s, cpu / gpu
  temperature; 0xFF = n/a) from `tools/sysstats.py` (psutil; NVML via nvidia-ml-py; ioreg
  on a Mac). Standby mode 2 "Dashboard" = an LVGL screen loaded instead of the main one
  (`ui_show_dashboard`): clock 48 px + date "Wednesday, 28 Oct" 20 px along the top edge (RTC
  date via `display_rtc_read_date`, read every 30 s), the next-meeting line under them (the
  reminder card is moved down over that line and narrowed while the dashboard is shown), RAM / VRAM as two ring gauges (custom draw: 240° arc from
  bottom-left, gradient = overlapping `lv_draw_arc` segments to the end + a faint wider glow),
  4 tiles CPU / GPU / NET / DISK with 120-point lv_chart sparklines, 1 px per point, two
  samples averaged per point = 2 min (ext y arrays, rate tiles autoscale). Values go dim 5 s after the last packet.
- `src/cal.*` — meetings from the PC: CTRL command 0x15 n × {id u16, start_in_min i16,
  dur_min u16, flags u8 (1 confirmed / 2 tentative), len u8, title ≤ 24 ASCII}. Times are
  relative minutes counted with millis(), so the adapter needs no date / time zone; the
  list is dropped 2 h after the last packet. Reminder = a top-layer card (`remindCard`,
  over the title row) from 15 min before to 5 min after the start: "in N min · title ·
  15:00 - 15:30 · ✕"; confirmed = opaque with an amber border, tentative = 60 % opacity.
  ✕ → `cal_dismiss()` → notification 0x20 <id u16>; the PC drops the meeting for the day
  (`dismissed.json`). A dark screen wakes up for a new reminder. Over the standby
  animation / live spectrum (LVGL paused) the card is an `lv_snapshot` (TRUE_COLOR_ALPHA,
  `LV_USE_SNAPSHOT 1` in the vendored lv_conf) handed to `src/overlay.*`, which blends it
  into every `display_push()` window that meets its rectangle (scratch copy, per-pixel
  alpha; the simulator's `display_push` does the same); `ui_reminder_loop()` returns true
  when the overlay was set / cleared and main.cpp then calls `live_redraw()` /
  `anim_redraw()`. A touch on the ✕ while LVGL gets no input (`ui_reminder_touch()` in
  pollTouch) dismisses without waking the screen.
- PC side of the dashboard / reminders: `tools/ble_live.py` is the one process (one GATT
  session) with tasks for the spectrum, `sysstats.py` (every 0.5 s) and `calendar_ics.py`
  (ICS URL / file — Google's "secret address in iCal format"; icalendar +
  recurring-ical-events; the next 3 meetings within 12 h; all-day / declined / cancelled
  skipped; the PARTSTAT of the attendee matching X-WR-CALNAME decides confirmed vs
  tentative; fetched every 5 min, the packet sent whenever it changes). Settings in
  `config.json` (`%LOCALAPPDATA%\uhk-ble-live` or `~/.config/uhk-ble-live`: audio, stats,
  calendar URL, calendar_on, agc); the tray menu switches write it and restart the stream,
  an edited file is read at the next (re)start. `ble_link.py` = `LinkBase` (commands,
  uploads, notification `listeners`) + `WinRtLink`; `ble_link_bleak.py` (`BleakLink`:
  Linux finds the paired adapter with `bluetoothctl devices Paired`, macOS needs
  `--address`) is picked automatically off Windows.
- Every touch / button interaction is listed in README "Using the screen": keep it in sync.
- **Do not change the GATT attribute table** (adding a characteristic to the OTA service
  broke it, 2026-10-27): Windows caches the table of a bonded device; after the change
  uncached characteristic discovery of that service threw "device does not recognize the
  command" (also after an adapter reset) — only re-pairing would fix it. Reverting the table
  fixed it; `ble_link.py` falls back to Windows' cached characteristics, so an OTA still works
  in that state.
- `src/ui.*` — title = the active slot's BLE name ("UHK60 BLE n"; blue when the host is
  authenticated, neon while pairing, grey otherwise); slot selector on the right: a
  little PCB (custom draw on the 116 px strip object, no board outline; each slot's tap
  area is its whole row): a copper bus from a via at the top, one pad per slot branching
  left / right with a 45° jog and an "S1..S5"
  silkscreen label (gold pad = host paired, empty gold ring = free). The bus is lit neon
  from the via to the active slot, whose pad glows (rect shadow); on a tap the lit end runs
  down the bus with a spark (ease-in-out), the old pad fades, then the new one lights up
  and pops a little (ease-out-back). Chosen from 4 rendered styles (keycaps, terminal,
  PCB, hex) — the others are not in the code. Below the slot line: typing statistics
  (keys | clicks | mouse distance); slot line +
  "Pair" button (hold 1.5 s = pairing mode, tap during pairing = cancel, countdown shown);
  status lines, log tail. Pop-up cards on the top layer: pairing passkey, UHK Agent "allow
  config access?", meeting reminder; the settings menu and the rename panel are on the SYS
  layer: under an opaque full-screen object there LVGL skips the screen and the top layer
  (`lv_refr_get_top_obj` is applied to the sys layer only) — on the top layer every menu
  frame redrew the main screen's neon glows beneath (107 ms/frame).
  - Display driver (vendored `lib/LilyGo-AMOLED-Series`): the Plus' 1-bit SPI runs at
    80 MHz (`RM67162_AMOLED_SPI.freq`, was 40: a full screen took 51 ms; the panel takes
    it), blocking Arduino `SPIClass::writeBytes`, one full-screen LVGL buffer in PSRAM.
    `LV_SHADOW_CACHE_SIZE 64`, `LV_GRAD_CACHE_DEF_SIZE 4096`, `LV_INDEV_DEF_READ_PERIOD 10`.
    Measured with LVGL's monitor_cb: the menu scroll was 85–107 ms/frame for ~100 kpx,
    i.e. render-bound, not bus-bound. A DMA attempt (2026-10-29: IDF `spi_master`, two
    32-row strips in internal RAM, bounce buffers for PSRAM pushes) was reverted: it ran,
    but after every `esp_restart()` from the DMA firmware the next boot's SPI transfers
    never reached the panel (black screen, loop hung or crawled on timeouts; a hard reset
    recovered it; `periph_module_reset` of SPI3 / GDMA at init did not help). Assigning
    `monitor_cb` was also present in the hung builds — not separated out. Next step if
    retried: UART0 boot log, or a trace of the previous run in RTC_NOINIT memory.
  - Settings menu (`ui_open_menu()`): a short press of BOOT (released before the 1.5 s
    hold, not while pairing; a press that woke a dark screen / stopped the animation only
    wakes it), or a SHORT_CLICKED on the already active slot's pad. Contents:
    - the active slot's host name + Rename → full-screen lv_keyboard dialog (OK saves,
      the keyboard key cancels, "" = no name);
    - the standby animation switch (`AnimSettings::mode`);
    - day / night brightness sliders (saved on release, `anim_set_settings()`).
    Full screen, scrolls down (finger-sized controls). ✕ or BOOT closes it, and it closes by itself after 60 s idle
    (`SCREEN_DIM_MS`). While it is open no standby animation or live spectrum starts;
    starting pairing closes it.
  - Host names: `ble_hid_set_host_name()`, NVS "bleslots" keys n0..n4, at most
    `BLE_HOST_NAME_MAX` (20) ASCII characters. Shown in the title (amber) and in "waiting
    for <name>". Cleared when another host pairs on the slot or on forget-all; a host
    that moves to another slot takes its name along.
  README / Pages screenshots (`docs/img/screen-*.png`) are rendered from
  `src/ui.cpp` with LVGL on a PC (also `screen-live` from `src/live.cpp` with canned band
  levels, and `screen-matrix` = a matrix demo frame through uanm encode / decode): `python tools/ui_sim/render.py` (gcc/g++ — TDM-GCC here —
  Pillow, LVGL from .pio/libdeps; objects cached in .pio/ui_sim; scenes + stubbed status
  in `tools/ui_sim/sim.cpp`; __DATE__/__TIME__ = last commit time so renders are
  reproducible; also leaves anim-*.png slot-switch frames in .pio/ui_sim). Re-render
  after UI changes; not done in CI.
- Screen: full brightness from boot, dims to 20% after 1 min idle, off after 3 min (AMOLED
  burn-in); touch / keyboard or BLE state change / pop-up wakes it (instantly). Dimming and
  switching off fade over 2.5 s (`display_fade_to`): interpolated in perceived brightness
  (level^(1/2.2)) with a cubic ease-out, so the low end does not drop abruptly. A touch on
  a dark or fading-out screen only wakes it (LVGL input is suppressed until release), so
  nothing is tapped blindly.

## Troubleshooting notes
- Windows PC (ASUS ROG Z790 Maximus Dark Hero, Intel Wireless Bluetooth 8087:0036) did not
  list the adapter: the Intel BT driver had stopped receiving ANY LE advertisements (WinRT
  watcher running, 0 adverts). Fixed by the Bluetooth driver installer from the ASUS site.
  Quick check: a raw `BluetoothLEAdvertisementWatcher` scan (Python `bleak` / `winrt`)
  must see neighbouring advertisers; the adapter shows as "UHK60 BLE n", E8:EE:8F:47:xx:xx.
- iPhone: a mouse pointer needs AssistiveTouch enabled (iPad does not).

## Status / open items
- Works on Windows (bench setup, keyboard on the board's USB-C): keys, mouse, UHK Agent
  reads and writes the configuration over BLE.
- Re-test after re-pairing: macOS (keys + Agent), iOS (keys; pointer needs AssistiveTouch),
  Android / Pixel (only one HID service is used there — main service is first).
- Passkey pairing + identity check (security fixes after the adversarial review): Windows
  shows the PIN prompt and pairs with the code from the screen (flashed by OTA). Still to
  test: macOS / iOS / Android, reconnects with private addresses (Mac /
  iPhone), and that a host of another slot is dropped. Not done from that review: OTA
  image signing, flash encryption / secure boot.
- Enclosure (3D-printed): USB-C power jack, keyboard panel jack (GCT USB4085-GF-A); BOOT
  reachable at the bottom.
