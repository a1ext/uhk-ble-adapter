# PlatformIO extra script
#  - after every build: $BUILD_DIR/uhk-ble-adapter.bin = bootloader + partitions
#    + boot_app0 + firmware merged into one image to flash at 0x0 (web installer,
#    docs/manifest.json), with the same flash parameters `pio run -t upload` uses
#  - `pio run -t ota`: upload over Bluetooth (tools/ble_ota.py, Windows). The
#    uploader needs a Python with the WinRT projections (`pip install bleak`);
#    set UHK_OTA_PYTHON to use a specific one.
import os

Import("env")  # noqa: F821 (provided by PlatformIO)

board = env.BoardConfig()  # noqa: F821
images = " ".join('%s "%s"' % (offset, path) for offset, path in env.get("FLASH_EXTRA_IMAGES", []))  # noqa: F821

env.AddPostAction(  # noqa: F821
    "$BUILD_DIR/${PROGNAME}.bin",
    env.VerboseAction(  # noqa: F821
        '"$PYTHONEXE" "$OBJCOPY" --chip $BOARD_MCU merge_bin -o "$BUILD_DIR/uhk-ble-adapter.bin" '
        "--flash_mode ${__get_board_flash_mode(__env__)} --flash_freq ${__get_board_f_image(__env__)} "
        '--flash_size %s %s $ESP32_APP_OFFSET "$BUILD_DIR/${PROGNAME}.bin"'
        % (board.get("upload.flash_size", "detect"), images),
        "Merging into $BUILD_DIR/uhk-ble-adapter.bin",
    ),
)

python = os.environ.get("UHK_OTA_PYTHON", "python")
script = os.path.join(env.subst("$PROJECT_DIR"), "tools", "ble_ota.py")  # noqa: F821

env.AddCustomTarget(  # noqa: F821
    name="ota",
    dependencies="$BUILD_DIR/${PROGNAME}.bin",
    actions=['"%s" "%s" "$BUILD_DIR/${PROGNAME}.bin"' % (python, script)],
    title="BLE OTA",
    description="Upload the firmware to the adapter over Bluetooth (Windows)",
)
