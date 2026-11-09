"""Native macOS system audio loopback for tools/ble_live.py using CoreAudio's
CATapDescription + AudioHardwareCreateProcessTap (macOS 14.2+, no BlackHole or
virtual audio driver needed).

CoreAudio's TCC check (kTCCServiceAudioCapture) requires the responsible process
to be an application bundle whose Info.plist contains NSAudioCaptureUsageDescription.
On first run this module compiles a tiny background helper bundle
(~/.config/uhk-ble-live/UHKAudioTap.app, LSUIElement=true) and launches it via
LaunchServices (`open -g -n -a`), receiving mono float32 PCM over a Unix socket.
"""
import os
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path

import numpy as np

BUNDLE_ID = "com.uhk.ble.audiotap"
SOURCE_VERSION = "1"

INFO_PLIST = f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>UHKAudioTap</string>
    <key>CFBundleIdentifier</key>
    <string>{BUNDLE_ID}</string>
    <key>CFBundleName</key>
    <string>UHK BLE Audio Tap</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>{SOURCE_VERSION}</string>
    <key>CFBundleVersion</key>
    <string>{SOURCE_VERSION}</string>
    <key>LSUIElement</key>
    <true/>
    <key>NSAudioCaptureUsageDescription</key>
    <string>Captures system audio for the UHK BLE adapter live spectrum.</string>
</dict>
</plist>
"""

OBJC_SOURCE = r"""
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>

static int sock_fd = -1;
static atomic_bool should_stop = false;

static void on_sig(int sig) {
    (void)sig;
    atomic_store(&should_stop, true);
}

static OSStatus io_proc(AudioObjectID inDevice, const AudioTimeStamp *inNow,
                        const AudioBufferList *inInputData, const AudioTimeStamp *inInputTime,
                        AudioBufferList *outOutputData, const AudioTimeStamp *inOutputTime,
                        void *inClientData) {
    (void)inDevice; (void)inNow; (void)inInputTime;
    (void)outOutputData; (void)inOutputTime; (void)inClientData;
    if (!inInputData || inInputData->mNumberBuffers == 0)
        return 0;

    const AudioBuffer *b0 = &inInputData->mBuffers[0];
    if (!b0->mData || b0->mDataByteSize == 0)
        return 0;

    uint32_t ch = b0->mNumberChannels ? b0->mNumberChannels : 1;
    uint32_t frames = (b0->mDataByteSize / sizeof(float)) / ch;
    if (frames == 0 || frames > 8192)
        return 0;

    float mono[8192];
    if (inInputData->mNumberBuffers >= 2 && ch == 1) {
        const float *l = (const float *)inInputData->mBuffers[0].mData;
        const float *r = (const float *)inInputData->mBuffers[1].mData;
        for (uint32_t i = 0; i < frames; i++)
            mono[i] = 0.5f * (l[i] + r[i]);
    } else if (ch >= 2) {
        const float *src = (const float *)b0->mData;
        for (uint32_t i = 0; i < frames; i++)
            mono[i] = 0.5f * (src[i * ch] + src[i * ch + 1]);
    } else {
        memcpy(mono, b0->mData, frames * sizeof(float));
    }

    ssize_t n = send(sock_fd, mono, frames * sizeof(float), 0);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        atomic_store(&should_stop, true);
    return 0;
}

int main(int argc, const char *argv[]) {
    if (argc < 2)
        return 1;
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0)
        return 1;
    int one = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path) - 1);
    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock_fd);
        return 1;
    }

    @autoreleasepool {
        CATapDescription *desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
        desc.UUID = [NSUUID UUID];
        desc.privateTap = YES;

        AudioObjectID tapID = kAudioObjectUnknown;
        if (AudioHardwareCreateProcessTap(desc, &tapID) != noErr || tapID == kAudioObjectUnknown) {
            close(sock_fd);
            return 2;
        }

        AudioStreamBasicDescription asbd = {0};
        UInt32 sz = sizeof(asbd);
        AudioObjectPropertyAddress fmtAddr = {
            kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
        };
        uint32_t rate = 48000;
        if (AudioObjectGetPropertyData(tapID, &fmtAddr, 0, NULL, &sz, &asbd) == noErr && asbd.mSampleRate > 0)
            rate = (uint32_t)asbd.mSampleRate;

        if (send(sock_fd, &rate, sizeof(rate), 0) != sizeof(rate)) {
            AudioHardwareDestroyProcessTap(tapID);
            close(sock_fd);
            return 3;
        }
        fcntl(sock_fd, F_SETFL, fcntl(sock_fd, F_GETFL, 0) | O_NONBLOCK);

        NSDictionary *aggDict = @{
            @"name": @"uhk-ble-live-tap",
            @"uid": [[NSUUID UUID] UUIDString],
            @"private": @YES,
            @"tapautostart": @NO,
            @"taps": @[@{@"uid": [desc.UUID UUIDString], @"drift": @YES}]
        };
        AudioObjectID aggID = kAudioObjectUnknown;
        if (AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggDict, &aggID) != noErr) {
            AudioHardwareDestroyProcessTap(tapID);
            close(sock_fd);
            return 4;
        }

        AudioDeviceIOProcID procID = NULL;
        if (AudioDeviceCreateIOProcID(aggID, io_proc, NULL, &procID) == noErr &&
            AudioDeviceStart(aggID, procID) == noErr) {
            struct pollfd pfd = {.fd = sock_fd, .events = POLLIN | POLLHUP | POLLERR};
            while (!atomic_load(&should_stop)) {
                if (poll(&pfd, 1, 100) > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLIN)))
                    break;
            }
            AudioDeviceStop(aggID, procID);
        }
        if (procID)
            AudioDeviceDestroyIOProcID(aggID, procID);
        AudioHardwareDestroyAggregateDevice(aggID);
        AudioHardwareDestroyProcessTap(tapID);
    }
    close(sock_fd);
    return 0;
}
"""


def ensure_helper_app(base_dir: Path) -> Path:
    """Builds UHKAudioTap.app once so its code signature and TCC grant stay valid."""
    app_dir = base_dir / "UHKAudioTap.app"
    contents = app_dir / "Contents"
    macos_dir = contents / "MacOS"
    bin_path = macos_dir / "UHKAudioTap"
    ver_file = contents / ".version"

    if bin_path.exists() and ver_file.exists() and ver_file.read_text().strip() == SOURCE_VERSION:
        return app_dir

    macos_dir.mkdir(parents=True, exist_ok=True)
    (contents / "Info.plist").write_text(INFO_PLIST, encoding="utf-8")
    with tempfile.NamedTemporaryFile("w", suffix=".m", delete=False, encoding="utf-8") as src:
        src.write(OBJC_SOURCE)
        src_path = src.name
    try:
        subprocess.run(
            ["clang", "-O2", "-fobjc-arc", src_path, "-framework", "Foundation",
             "-framework", "CoreAudio", "-o", str(bin_path)],
            check=True, capture_output=True, text=True,
        )
        subprocess.run(
            ["codesign", "-s", "-", "-i", BUNDLE_ID, "--force", "--deep", str(app_dir)],
            check=True, capture_output=True, text=True,
        )
        ver_file.write_text(SOURCE_VERSION, encoding="utf-8")
    finally:
        try:
            os.unlink(src_path)
        except OSError:
            pass
    return app_dir


def capture_macos(args, out, stop: threading.Event, analyzer_cls, rolling_cls, base_dir: Path):
    """Captures macOS system audio via UHKAudioTap.app and feeds FFT band levels into `out`."""
    app_dir = ensure_helper_app(base_dir)
    while not stop.is_set():
        with tempfile.TemporaryDirectory(prefix="uhk-tap-") as tmp:
            sock_path = os.path.join(tmp, "tap.sock")
            srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            srv.bind(sock_path)
            srv.listen(1)
            srv.settimeout(10.0)
            try:
                subprocess.run(["open", "-g", "-n", "-a", str(app_dir), "--args", sock_path], check=True)
                conn, _ = srv.accept()
            except Exception as exc:
                srv.close()
                if not stop.is_set():
                    print(f"macOS audio tap failed to start ({exc}); retrying", flush=True)
                    time.sleep(1.0)
                continue
            srv.close()

            with conn:
                conn.settimeout(30.0)
                try:
                    hdr = conn.recv(4)
                    if len(hdr) < 4:
                        raise RuntimeError("audio tap closed before sending sample rate")
                    samplerate = struct.unpack("<I", hdr)[0] or args.samplerate
                    hop = int(round(samplerate / args.fps))
                    an = analyzer_cls(samplerate, args.fft, args.bands, args.fmin, args.fmax,
                                      args.floor, args.ceil, args.tilt, args.gate, args.agc, args.fps)
                    roll = rolling_cls(args.fft)
                    zeros = np.zeros(hop, dtype=np.float32)
                    args.analyzer = an
                    print(f"loopback: macOS CoreAudio Process Tap @ {samplerate} Hz | window {args.fft} "
                          f"({1000 * args.fft / samplerate:.0f} ms) | {args.bands} bands @ {args.fps:.0f} fps")

                    conn.setblocking(False)
                    period = 1.0 / args.fps
                    next_tick = time.perf_counter() + period
                    last_audio = time.perf_counter()
                    rem = b""
                    while not stop.is_set():
                        wait = max(0.0, next_tick - time.perf_counter())
                        r, _, _ = select.select([conn], [], [], wait)
                        if r:
                            chunk = conn.recv(65536)
                            if not chunk:
                                raise RuntimeError("audio tap stream ended")
                            data = rem + chunk
                            n_floats = len(data) // 4
                            rem = data[n_floats * 4:]
                            if n_floats:
                                roll.push(np.frombuffer(data[:n_floats * 4], dtype=np.float32))
                                last_audio = time.perf_counter()
                        now = time.perf_counter()
                        if now >= next_tick:
                            if now - last_audio > 0.15:
                                roll.push(zeros)
                            out.put(an.levels(roll.buf))
                            next_tick += period
                            if now >= next_tick:
                                next_tick = now + period
                except Exception as exc:
                    if not stop.is_set():
                        print(f"capture dropped ({exc}); reopening", flush=True)
                        time.sleep(0.5)
