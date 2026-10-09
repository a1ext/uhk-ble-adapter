# UHK 60 v2 → BLE adapter

A DIY USB→Bluetooth adapter for the Ultimate Hacking Keyboard 60 v2, built on a
LilyGo T-Display-S3 AMOLED Plus. Hardware and firmware details are in
[CLAUDE.md](CLAUDE.md).

<p align="center">
  <img src="docs/img/screen-bt.png" width="564" alt="Adapter screen in Bluetooth mode: Cable / Bluetooth switch, keyboard and connection status, and the slot selector with slot 3 active">
</p>

## What it does

- **Up to 5 Bluetooth hosts.** Every slot is a Bluetooth device of its own
  ("UHK60 BLE 1" … "UHK60 BLE 5") with its own address. Each computer, tablet
  or phone pairs with its own slot and reconnects to it by itself. To move the
  keyboard to another host, tap that host's leaf on the stem.
- **UHK Agent over Bluetooth.** The adapter passes the keyboard's Agent
  command channel through as a separate Bluetooth HID device that identifies
  as a UHK 60 v2. Agent finds the keyboard and reads and writes its keymaps
  and macros without a cable. As far as we can tell from Agent's sources, it
  configures even the Bluetooth-capable UHK 80 only over USB. Keyboard
  firmware updates still need the cable.
- **Cable or Bluetooth, one tap.** In Cable mode an analog USB switch connects
  the keyboard straight to the computer, and the adapter is out of the data
  path. In Bluetooth mode the board is the keyboard's USB host and acts as a
  Bluetooth keyboard and mouse for the computer.
- **Everything the keyboard sends, unchanged.** The Bluetooth HID layout is
  built from the keyboard's own USB descriptors: NKRO, mouse keys with smooth
  scrolling, media and system keys. Keystrokes are never merged, so fast
  macros arrive intact.
- **Secure by default.** Pairing needs the 6-digit code shown on the
  adapter's screen, and only the active slot's host gets keystrokes. Agent can
  read status freely; reading or changing the configuration needs a tap on
  **Allow** on the adapter.
- **Wireless updates of the adapter.** New firmware goes over Bluetooth into
  the second app slot while the keyboard keeps typing. If the new firmware
  fails before a host reconnects, the adapter rolls back to the previous one.

<table>
  <tr>
    <td><img src="docs/img/screen-passkey.png" width="400" alt="Pairing pop-up showing the 6-digit code 482 913"></td>
    <td><img src="docs/img/screen-agent.png" width="400" alt="Pop-up: UHK Agent asks for config access, with Deny and Allow buttons"></td>
  </tr>
  <tr>
    <td align="center">Pairing a new host: type the code on it</td>
    <td align="center">UHK Agent asks before touching the config</td>
  </tr>
</table>

## Flashing

The adapter's screen shows which firmware is running: the line above the log
starts with `fw <build date> <build time>`.

### Fresh board: web installer

**<https://a1ext.github.io/uhk-ble-adapter/>** flashes the firmware straight
from the browser. You don't need to install any tools.

You need:
- a LilyGo T-Display-S3 AMOLED Plus and a USB-C cable that carries data (some
  charging cables don't);
- Chrome, Edge or Opera on a desktop computer (Windows, macOS, Linux). The page
  talks to the board over Web Serial, which Firefox, Safari and mobile browsers
  don't support.

Steps:
1. Plug the board's USB-C straight into the computer, without a hub if you can.
2. Open the page and click **Install firmware**.
3. Pick the board's port. It's usually called "USB JTAG/serial debug unit"
   (a COM port on Windows, `/dev/cu.usbmodem…` on macOS).
4. Tick **Erase device** on the first install and confirm. Flashing takes
   about a minute.
5. When the installer reports success, press **Reset** once if the screen
   stays black. The installer's own reset can leave the chip in its
   bootloader.

Then set it up:
1. Switch to **Bluetooth** on the touch screen and plug the keyboard into the
   adapter. The adapter reads the keyboard's layout once and remembers it.
2. Pick a slot on the stem on the right, then hold **BOOT** for 1.5 s, until
   the bar along the bottom edge fills.
3. Within 20 s, add **UHK60 BLE n** on the computer or phone, and type the
   6-digit code that the adapter shows.

If something goes wrong:
- **No port in the list:** put the board into its bootloader. Hold **BOOT**,
  press and release **Reset**, release **BOOT**, then click **Install
  firmware** again. Press **Reset** after flashing.
- **"Failed to open serial port":** another program is using the port. Close
  serial monitors, PlatformIO and other installer tabs.
- **The adapter is in Bluetooth mode:** its USB port then acts as the
  keyboard's USB host, so the computer sees no serial port. Switch to
  **Cable** on the screen and press **Reset**, or use the bootloader as above.
- **The adapter is assembled** and its USB-C is wired to the keyboard: the
  browser can't reach the board. Update it over Bluetooth (below) or over UART.

Reinstalling later works the same way. Untick **Erase device** to keep the
paired hosts and the cached keyboard layout. For routine updates, use
Bluetooth (below).

The page always installs the latest build of `master`; its version is shown
next to the button. Fixed versions are on the
[Releases page](https://github.com/a1ext/uhk-ble-adapter/releases):
`*-full.bin` is the complete image, written at 0x0 with any esptool
(`esptool.py --chip esp32s3 write_flash 0x0 uhk-ble-adapter-v1.0-full.bin`).
`*-ota.bin` is the app image for the Bluetooth uploader.

Maintainers: CI (`.github/workflows/build.yml`) builds the site from `docs/`
plus the firmware on every push to `master` and deploys it to GitHub Pages
(**Settings → Pages → Source: GitHub Actions**). Version tags (`v*`) also
create a release. For tags to deploy too, allow `v*` under **Settings →
Environments → github-pages**.

### Over Bluetooth (OTA, from Windows) — the usual way

No cables, no mode switching: the new image is written to the second app slot
while the adapter keeps running, so the keyboard keeps typing.

Requirements:
- the adapter is in **Bluetooth** mode, paired with this PC and connected to it;
- a Python with the WinRT Bluetooth bindings: `pip install bleak`. To use a
  different interpreter, point the `UHK_OTA_PYTHON` environment variable at it.

```bash
pio run -t ota
```

The same without the PlatformIO target:

```bash
python tools/ble_ota.py .pio/build/t-display-s3-amoled-plus/firmware.bin
```

What happens:
1. The firmware is built, then the adapter erases the second slot (a few seconds).
2. Sending ~1.4 MB takes about 35 s (~43 KB/s). The adapter's screen shows the
   progress: a bar along the bottom edge and a "Firmware update N%" line.
3. The adapter verifies the image (SHA-256 and format) and reboots into the new
   firmware. The keyboard does not hop over to the PC meanwhile.
4. The new firmware is confirmed once Windows has reconnected (the log shows
   `OTA: new firmware confirmed`) or after 5 minutes of uptime. If it crashes or
   resets before that, the bootloader brings the previous firmware back.

Only the host paired on the active slot can flash the adapter over Bluetooth:
OTA requires its passkey-authenticated, encrypted connection.

### Over USB

```bash
pio run -t upload
```

- The board's USB-C must be plugged into the PC and the adapter must be in
  **Cable** mode. In Bluetooth mode the board's USB port is the keyboard's USB
  host and there is no COM port: switch to Cable and press Reset.
- If the firmware does not start or the port does not show up, use the
  bootloader: hold BOOT, press Reset, release BOOT.
- The board restarts by itself after flashing.

### Over UART (assembled adapter, the board's USB-C is taken by the keyboard)

Connect a 3.3 V USB-UART adapter to CN1 (JST-SH 1.0, 4 pins: GND / 3V3 / TX / RX),
enter the bootloader (BOOT + Reset) and run:

```bash
pio run -t upload --upload-port COMx
```

### Troubleshooting

- **"OTA service not found"** — the adapter runs firmware without OTA (flash it
  over USB the first time), or Windows cached an old service list: remove the
  adapter in the Windows Bluetooth settings and pair it again.
- **"no paired device named 'UHK60 BLE*'"** — the adapter is not paired: in
  Bluetooth mode hold BOOT for 1.5 s (bar along the bottom of the screen), then
  add the device in Windows within 20 s and type the 6-digit code the adapter
  shows.
- **"OTA service not found (status 1)"** with "not connected" — Windows has no
  link to the adapter right now: Bluetooth mode, the slot of this PC selected,
  the keyboard typing on the PC.
- **After a firmware update all hosts must pair again** when the release notes
  say so (pairing format changes): remove "UHK60 BLE n" on the host, then pair
  it again as above.
- **Windows does not see any Bluetooth devices** — on the development PC the
  Intel Bluetooth driver had stopped receiving advertisements; the driver
  installer from the ASUS site fixed it.
- **UHK Agent says it cannot read / write the configuration** — over Bluetooth
  the adapter only lets status commands through by default. Tap **Allow** in
  the "UHK Agent asks for config access" pop-up on the adapter's screen, then
  retry in Agent; access stays open for 10 minutes after the last change and
  closes when the host disconnects ("Agent open … - tap to lock" closes it now).
- **The UHK keyboard's own firmware** cannot be updated over Bluetooth: the
  adapter refuses those Agent commands. Update it in Cable mode.
