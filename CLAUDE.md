# UHK 60 v2 → BLE adapter (DIY)

## Goal
Homemade USB→Bluetooth adapter for an Ultimate Hacking Keyboard 60 v2, with a
mode switch:
- **Cable mode** — keyboard D+/D- routed directly to the PC (ESP not in path; UHK Agent works).
- **BT mode** — ESP32-S3 acts as USB host for the UHK and as a BLE HID device to the PC.
Power always comes from the input USB cable (PC or charger).

## Hardware decisions
- MCU board: **LilyGO T-Display S3 AMOLED Plus** (ESP32-S3R8, single USB-C = native USB).
  Schematic: `../../Lilygo/LilyGo-AMOLED-Series/schematic/T-Display-S3-AMOLED-Plus.pdf`.
  - Its USB-C is used as the **USB host port** via a short USB-C plug breakout ("pigtail"):
    5V/GND in (powers the board) + D+/D- (host data). GPIO19/20 are not on the headers.
  - USB-C VBUS and the header **VBUS pins are the same net** (only a TVS). Never have the
    board's USB-C in a PC while 5V is fed into the header VBUS. CC1/CC2 have 5.1k Rd on board.
  - PMU is BQ25896 (PSEL variant, does not touch D+/D-); the LilyGo lib disables charging/OTG.
  - UART0 (GPIO43 TX / GPIO44 RX) is on **CN1, JST-SH 1.0 4-pin (GND/3V3/TX/RX)** and on
    the header: console + flashing in the assembled adapter (BOOT + RST → ROM download over
    UART or USB, whichever is connected).
- Data-line switch: **analog USB 2.0 2:1 mux**, candidates **CH442E** or **FSUSB42MUX**
  (MSOP-10, hand-solderable; TS3USB221 also fine but QFN).
  - COM = UHK D+/D-; port 1 = PC cable (SEL=0); port 2 = ESP pigtail (SEL=1).
  - VCC from board 3V3, EN tied active, 100 nF decoupling.
  - **SEL pulled down (10k) → defaults to cable mode** on reset / dead firmware / ROM bootloader.
- GPIO assignment:
  - **GPIO16 → mux SEL** (output, 10k to GND).
  - Mode (cable / BT) is switched on the touch screen. The physical button — BOOT (GPIO0)
    and an optional panel button on **GPIO10 → GND** (internal pull-up) — is the pairing
    button: hold 1.5 s = pairing mode for the active slot, press while pairing = cancel.
    (BOOT held during reset still enters the ROM bootloader.)
  - Spare: GPIO39–42 (header).
  - Taken / avoid: 0, 3, 45, 46 strapping; 1 PMU IRQ; 2/3 I2C (touch, RTC, PMU); 4 battery
    divider; 5, 6, 7, 9, 17, 18, 38, 47, 48 display; 11–14 SD card (11 also touch, per user);
    15 RTC INT; 21 touch INT; 19/20 USB; 43/44 UART0.
- Only VBUS, GND, D+, D- are needed (SuperSpeed lines unused). Simple USB 2.0 USB-C panel jacks;
  the PC-side jack needs 5.1k Rd on CC1 and CC2 for C-to-C cables.
- Power input: polyfuse ~1 A, 100–220 µF bulk cap near the keyboard connector.

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
- USB / UART: `pio run -t upload` (board in cable mode, or BOOT + RST).
- **Bluetooth (Windows): `pio run -t ota`** → `tools/ble_ota.py` uploads `firmware.bin` to the
  paired adapter through Windows' own GATT connection (no advertising needed). Needs a
  Python with the WinRT projections (`pip install bleak`); `UHK_OTA_PYTHON` selects the
  interpreter. `src/ota.*`: custom GATT service (MITM-encrypted link of the active slot's
  authenticated host only, `ble_hid_ready()` checked on every write), the image goes
  straight into the inactive app partition (A/B, default_16MB.csv app0/app1) while the
  firmware keeps running; SHA-256 + esp_ota_end validation, then boot partition switch and
  reboot (SEL held in BT mode). Rollback: `verifyRollbackLater()` returns true, the new
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
  Installed lazily on the first switch to BT mode (until then USB flashing works).
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
    are always refused — firmware updates by cable only.
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
- Mode in NVS; crash / WDT / brownout reset → cable mode. Before a deliberate restart in BT
  mode SEL is latched with `gpio_hold_en` so the keyboard does not hop to the PC.
- `src/ui.*` — iOS-style Cable / Bluetooth switch (tapping a mode label works too; the mode
  is applied 250 ms later so the animation stays smooth); slot selector on the right: a
  vertical stem with one triangular leaf per slot (tip on the stem, wide edge outward),
  alternating left / right (custom
  draw, `lv_draw_polygon`; grey = host paired, dark = free); the active one is a neon
  pink→violet gradient with a layered glow; on a tap a spark runs along the stem, then the
  new leaf pops out of it (ease-out-back); slot line +
  "Pair" button (hold 1.5 s = pairing mode, tap during pairing = cancel, countdown shown);
  status lines, log tail. Strip and pairing controls are hidden in cable mode. Pop-up cards
  on the top layer: pairing passkey, UHK Agent "allow config access?".
- Screen: full brightness from boot, dims to 20% after 1 min idle, off after 3 min (AMOLED
  burn-in); touch / keyboard or BLE state change / pop-up wakes it (instantly). Dimming and
  switching off fade over 2.5 s (`display_fade_to`): interpolated in perceived brightness
  (level^(1/2.2)) with a cubic ease-out, so the low end does not drop abruptly. A touch on
  a dark or fading-out screen only wakes it (LVGL input is suppressed until release), so
  it cannot flip the switch blindly.

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
- First real BLE OTA run (`pio run -t ota`).
- Exact mux pinout (FSUSB42MUX is the part with stock at TME; check its datasheet).
- Enclosure (3D-printed), two USB-C panel jacks (GCT USB4085-GF-A) + button.
