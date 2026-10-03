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
- `src/driver.c`, `src/ds4bt.inf`: **not compiled or run yet** (needs the WDK on Windows and a real pad).

## Build (Windows)
VS 2022 + WDK. Create a "Kernel Mode Driver, Empty (KMDF)" project named `ds4bt`, then add `src/*.c`, `src/*.h` and
`src/ds4bt.inf`.

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

## Not done
- Speaker (BT audio reports 0x14-0x19 + SBC; needs a separate virtual audio driver).
- XInput-only games still need ViGEm or Steam Input.
- No recovery timer if a lower read fails (see the `ponytail:` note in `IssueRead`).
