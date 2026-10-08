# PlatformIO extra script: `pio run -t ota` builds and uploads over Bluetooth
# (tools/ble_ota.py, Windows). The uploader needs a Python with the WinRT
# projections (`pip install bleak`); set UHK_OTA_PYTHON to use a specific one.
import os

Import("env")  # noqa: F821 (provided by PlatformIO)

python = os.environ.get("UHK_OTA_PYTHON", "python")
script = os.path.join(env.subst("$PROJECT_DIR"), "tools", "ble_ota.py")  # noqa: F821

env.AddCustomTarget(  # noqa: F821
    name="ota",
    dependencies="$BUILD_DIR/${PROGNAME}.bin",
    actions=['"%s" "%s" "$BUILD_DIR/${PROGNAME}.bin"' % (python, script)],
    title="BLE OTA",
    description="Upload the firmware to the adapter over Bluetooth (Windows)",
)
