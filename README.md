# ds4bt: Bluetooth DualShock 4 that behaves like a USB one (Windows)

A KMDF lower filter that sits below `HidBth.sys` and rewrites L2CAP HID payloads, so games see
USB reports (input 0x01, output 0x05 for rumble and light bar, calibration feature 0x02).
Plan: `~/.claude/plans/currently-when-connected-over-giggly-map.md`.

## Status
- `src/ds4_translate.c`: BT<->USB report translation and CRC32. Tested:
  `gcc -I src src/ds4_translate.c test/test_translate.c -o t && ./t`
- `src/driver.c` and `src/ds4bt.inf`: the filter. **Not compiled yet** (needs the WDK on Windows).
- **Missing: descriptor substitution (Phase 0).** HidBth must report the USB DS4 descriptor, or HidClass
  rejects the translated reports. So `Translate` defaults to 0 and the filter only logs.

## Build (Windows)
VS 2022 + WDK. Create a "Kernel Mode Driver, Empty (KMDF)" project named `ds4bt`, then add `src/*.c`, `src/*.h`, and `src/ds4bt.inf`.
Link `$(DDK_LIB_PATH)` default libs only; Bluetooth headers are in the WDK.

## Install (test machine)
```
bcdedit /set testsigning on            (reboot)
pnputil /add-driver ds4bt.inf /install
```
Disable and re-enable the DS4 in Device Manager (or re-pair it). To see the logs, use DebugView with
"Capture Kernel" enabled, or WinDbg with `ed nt!Kd_IHVDRIVER_Mask 0xF`.

## Phase 0: what to do with the logs
1. Confirm that `brb type` lines appear at connect time (ACL data transfers aren't logged, to avoid flooding at 250 Hz).
2. Work out how HidBth gets the HID descriptor. Watch for unknown `internal ioctl` / `brb type` lines at connect time.
   Also check `HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices\<mac>\CachedServices`.
3. Capture the USB DS4 descriptor (on Linux: `cat /sys/class/hidraw/hidrawN/device/report_descriptor > ds4_usb.bin`
   with the pad plugged in by cable), then substitute it at the point found in step 2.
4. Set `HKLM\SYSTEM\CurrentControlSet\Services\ds4bt\Parameters\Translate = 1` and restart the device.
