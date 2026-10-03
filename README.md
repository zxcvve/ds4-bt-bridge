# ds4bridge: Bluetooth DualShock 4 that behaves like a USB one (Windows)

Over Bluetooth, Windows exposes the DS4 with its Bluetooth report layout (input 0x11, output 0x11 + CRC32).
Games, SDL, Steam and browsers speak the USB layout (input 0x01, output 0x05), so rumble and the light bar
never reach the pad. `bridge/ds4bridge.c` translates in user mode (no test signing, Secure Boot stays on): it reads
the Bluetooth pad with hidapi and feeds a ViGEmBus virtual DS4 v2 (`054C:09CC`, USB layout); rumble and light bar
written to the virtual pad go back to the real one.

## Status
- `bridge/ds4_translate.c`: tested: `gcc -I bridge bridge/ds4_translate.c test/test_translate.c -o t && ./t`
- `bridge/config.c`: tested: `gcc -I bridge bridge/config.c test/test_config.c -o tc && ./tc`
- `bridge/imu.c`: tested: `gcc -I bridge bridge/imu.c test/test_imu.c -lm -o ti && ./ti`
- `bridge/stats.c`: tested: `gcc -I bridge bridge/stats.c test/test_stats.c -lm -o ts && ./ts`
- `bridge/sbc.c`: tested: `gcc -I bridge bridge/sbc.c test/test_sbc.c -lm -o tsbc && ./tsbc`

## Build and run
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
The window title shows the battery level. PS + Triangle on the pad turns it off, like in Steam: the bridge drops
its Bluetooth link (the pad powers off on that) and exits. `+`/`-` in the bridge window change the light bar brightness in 10% steps
(0 turns it off to save battery). The starting brightness and the color used until a game sets one are in
`config.toml` next to `ds4bridge.exe` (template: `bridge/config.toml`); `--brightness N` overrides the file.
`report_rate` there (250, 500 or 1000 Hz, SDL's values) sets the pad's Bluetooth input rate; unset keeps the pad's default.
`s` toggles link stats, printed once a second: input report rate, longest gap, delivery delay (how much later than
the second's fastest report each one arrived, judged by the pad's own timestamps; constant latency isn't visible),
packets dropped as bad, and how long writes to the pad (rumble, LED, audio) take.
`t` toggles a 1 kHz test tone on the pad's speaker (SBC in output report 0x17, layout from public
captures, confirmed on hardware).
`audio_device` in `config.toml` (part of an output device's name, e.g. `"CABLE Input"`) plays what plays on that
device on the pad's speaker: the bridge records it with WASAPI loopback, no driver of its own. With
[VB-Audio Virtual Cable](https://vb-audio.com/Cable/) set as a game's output, only the pad plays it; naming your
speakers makes the pad play along. A wrong name stops audio only and prints the device names.
Audio goes to headphones plugged into the pad's jack, to the speaker otherwise. `volume` in `config.toml` (0-100,
default 64) sets both; the `[` and `]` keys in the bridge window (any keyboard layout) change it in steps of 10 (shown in the window title).
The virtual pad's descriptor and feature reports come from ViGEmBus, not from the real pad. Its gyro/accel
calibration (feature 0x02) is another pad's, fixed in ViGEmBus, so the bridge remaps the real pad's raw IMU values
onto it with SDL's math. Like SDL, it drops Bluetooth packets without the HID flag or, once CRCs prove reliable,
with a bad CRC.

## Not done
- Speaker: no mic; volume 0-100 is sent raw, the pad's real range is unknown; audio stops for good if the device goes away or its format
  changes, until the bridge restarts.
- XInput-only games still need ViGEm or Steam Input.
