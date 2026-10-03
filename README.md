# ds4bt: Bluetooth DualShock 4 that behaves like a USB one (Windows)

Over Bluetooth, Windows exposes the DS4 with its Bluetooth report layout (input 0x11, output 0x11 + CRC32).
Games, SDL, Steam and browsers speak the USB layout (input 0x01, output 0x05), so rumble and the light bar
never reach the pad. This driver makes the Bluetooth pad look exactly like a USB one.

## How it works
```
HidClass + mshidkmdf    function driver on the DS4's HID collection (HID\{00001124-...}_VID&0002054c_PID&09cc)
ds4bt.sys               lower filter: serves the USB report descriptor, translates reports
HidBth.sys              Windows' Bluetooth HID transport (unchanged)
```
- `IOCTL_HID_GET_REPORT_DESCRIPTOR` returns the USB DS4 v2 descriptor (`src/ds4_usb_descriptor.h`).
- Reads: BT 0x11 becomes USB 0x01. Writes: USB 0x05 becomes BT 0x11 with CRC. Feature 0x02 (calibration) becomes BT 0x05.
- At start the driver reads feature 0x05 once, which switches the pad to full 0x11 reports (gyro and touchpad).

Design taken from [imbushuo/mac-precision-touchpad](https://github.com/imbushuo/mac-precision-touchpad)
`src/AmtPtpHidFilter`, which does the same for the Magic Trackpad 2 over Bluetooth.

**Caveat:** HidClass owns HidBth's dispatch table, so the driver patches
`HidBth.sys`'s `IRP_MJ_INTERNAL_DEVICE_CONTROL` back to HidBth's own handler (`DetourHidBth` in `src/driver.c`).
That patch is undocumented, applies to every Bluetooth HID device, and can break with a Windows update.

## Status
- `src/ds4_translate.c`: tested: `gcc -I src src/ds4_translate.c test/test_translate.c -o t && ./t`
- `bridge/config.c`: tested: `gcc -I bridge bridge/config.c test/test_config.c -o tc && ./tc`
- `bridge/imu.c`: tested: `gcc -I bridge bridge/imu.c test/test_imu.c -lm -o ti && ./ti`
- `src/driver.c`, `src/ds4bt.inf`: **not compiled or run yet** (needs the WDK on Windows and a real pad).

## Build (Windows)
Use either VS 2022 ("Desktop development with C++", Spectre-mitigated libs) plus a WDK that matches the installed SDK,
or the EWDK (run from its `LaunchBuildEnv.cmd` prompt). Then run:
```
powershell -ExecutionPolicy Bypass -File build.ps1 [-Configuration Debug]
```
It checks every prerequisite, reports what's missing, then builds and test-signs into `x64\<Configuration>\ds4bt\`.

## Install (test machine; set up a kernel debugger or a restore point first)
```
bcdedit /set testsigning on            (reboot)
pnputil /add-driver ds4bt.inf /install
```
Then disconnect and reconnect the pad. Check Device Manager to confirm that its HID entry now reads "DualShock 4 (Bluetooth, USB mode)".
If it doesn't, compare its Hardware Ids against `[Models.NTamd64.10.0]` in the INF.
Logs: DebugView with "Capture Kernel" enabled (`ds4bt:` prefix).

## Verify
- `hidapitester --vidpid 054C:09CC --list-detail`: the report sizes match USB (64-byte input, 32-byte output).
- `hidapitester --vidpid 054C:09CC --open --send-output 5,0xF7,4,0,255,255,0,0,255`: the motors spin and the LED turns blue.
- SDL `testcontroller`: rumble, LED, gyro and touchpad all work.
- Run Driver Verifier on `ds4bt.sys`, then disconnect/reconnect and sleep/resume: no bugcheck.

## Alternative: user-mode bridge (no test signing, Secure Boot stays on)
`bridge/ds4bridge.c` does the same translation in user mode: it reads the Bluetooth pad with hidapi and
feeds a ViGEmBus virtual DS4 v2 (`054C:09CC`, USB layout); rumble and light bar written to the virtual pad
go back to the real one. Don't install `ds4bt.sys` alongside it: the bridge expects the Bluetooth layout.

Needs [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases) and [HidHide](https://github.com/nefarius/HidHide/releases)
(both signed, both retired upstream). Building it needs no WDK: VS 2022 or newer ("Desktop development with C++")
and Git; CMake fetches hidapi and ViGEmClient.
```
powershell -ExecutionPolicy Bypass -File bridge\build.ps1 [-Configuration Debug]
```
Run `bridge\build\Release\ds4bridge.exe` (no elevation needed). While it runs, it hides the real pad through
HidHide's driver, so games see only the virtual one; on exit (Ctrl+C, closing the window, pad disconnect) it unhides it.
Start the bridge before games and Steam: HidHide only blocks new opens, so an app that already has the real pad open keeps it.
If the bridge is killed or crashes, the pad stays hidden until the bridge's next clean exit.
The window title shows the battery level. `+`/`-` in the bridge window change the light bar brightness in 10% steps
(0 turns it off to save battery). The starting brightness and the color used until a game sets one are in
`config.toml` next to `ds4bridge.exe` (template: `bridge/config.toml`); `--brightness N` overrides the file.
The virtual pad's descriptor and feature reports come from ViGEmBus, not from the real pad. Its gyro/accel
calibration (feature 0x02) is another pad's, fixed in ViGEmBus, so the bridge remaps the real pad's raw IMU values
onto it with SDL's math. Like SDL, it drops Bluetooth packets without the HID flag or, once CRCs prove reliable,
with a bad CRC.

## Not done
- Speaker (BT audio reports 0x14-0x19 + SBC; needs a separate virtual audio driver).
- XInput-only games still need ViGEm or Steam Input.
- No recovery timer if a lower read fails (see the `ponytail:` note in `IssueRead`).
