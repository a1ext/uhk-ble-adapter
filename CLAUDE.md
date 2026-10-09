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
    1.5 s = pairing mode for the active slot, press while pairing = cancel. (BOOT held
    during power-up / reset still enters the ROM bootloader.) No extra buttons.
  - Spare: GPIO10, GPIO16, GPIO39–42 (header).
  - Taken / avoid: 0, 3, 45, 46 strapping; 1 PMU IRQ; 2/3 I2C (touch, RTC, PMU); 4 battery
    divider; 5, 6, 7, 9, 17, 18, 38, 47, 48 display; 11–14 SD card (11 also touch, per user);
    15 RTC INT; 21 touch INT; 19/20 USB; 43/44 UART0.
- Power input: USB 2.0 USB-C receptacle (5.1k Rd on CC1 and CC2 for C-to-C cables, D+/D-
  unconnected) → polyfuse ~1 A → header VBUS; 100–220 µF bulk cap near the board.
- Wiring / pinout pictures for users: `docs/img/pinout.svg`, `docs/img/wiring.svg` (shown on
  the Pages site and in the README).

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
  version = `git describe`), tags `v*` → GitHub release with `-full.bin` and `-ota.bin`.
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
- `src/ui.*` — title = the active slot's BLE name ("UHK60 BLE n"; blue when the host is
  authenticated, neon while pairing, grey otherwise); slot selector on the right: a
  little PCB (custom draw on the strip object): black solder mask, a copper bus from a
  via at the top, one pad per slot branching left / right with a 45° jog and an "S1..S5"
  silkscreen label (gold pad = host paired, empty gold ring = free). The bus is lit neon
  from the via to the active slot, whose pad glows (rect shadow); on a tap the lit end runs
  down the bus with a spark (ease-in-out), the old pad fades, then the new one lights up
  and pops a little (ease-out-back). Chosen from 4 rendered styles (keycaps, terminal,
  PCB, hex) — the others are not in the code. Below the slot line: typing statistics
  (keys | clicks | mouse distance); slot line +
  "Pair" button (hold 1.5 s = pairing mode, tap during pairing = cancel, countdown shown);
  status lines, log tail. Pop-up cards on the top layer: pairing passkey, UHK Agent "allow
  config access?". README / Pages screenshots (`docs/img/screen-*.png`) were rendered from
  `src/ui.cpp` with LVGL on a PC (TDM-GCC, stubbed status functions, canned data).
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
