# ds4bridge: Bluetooth DualShock 4 that behaves like a USB one (Windows)

Connect your DualShock 4 over Bluetooth and games treat it like a pad plugged in by USB: rumble, the light bar and
the speaker all work. It runs as a normal program in user mode, so you don't need test signing and Secure Boot
stays on.

## Why it's needed

Over Bluetooth, Windows exposes the DS4 with its Bluetooth report layout (input 0x11, output 0x11 + CRC32).
Games, SDL, Steam and browsers speak the USB layout (input 0x01, output 0x05). As a result, rumble and light bar
commands never reach the pad. The bridge translates between the two.

## Requirements

- [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases) and [HidHide](https://github.com/nefarius/HidHide/releases).
  Both are signed, and both are retired upstream.
- To build: Visual Studio 2022 or newer with "Desktop development with C++", and Git. You don't need the WDK:
  CMake fetches hidapi and ViGEmClient itself.

## Build

```
powershell -ExecutionPolicy Bypass -File bridge\build.ps1 [-Configuration Debug]
```

## Run

1. Pair the pad with Windows over Bluetooth.
2. Start the bridge **before** games and Steam: `bridge\build\Release\ds4bridge.exe` (no admin rights needed).
   HidHide only blocks new opens, so an app that already has the real pad open keeps using it.
3. Play. Games see only the virtual pad.

While it runs, the bridge hides the real pad through HidHide. When it exits cleanly (Ctrl+C, closing the window,
or the pad disconnecting), it unhides the pad again. If the bridge is killed or crashes, the pad stays hidden
until the bridge's next clean exit.

The window title shows the battery level and the volume.

## Controls

| Input | What it does |
|---|---|
| PS + Triangle (on the pad) | Turns the pad off, like in Steam: the bridge drops the Bluetooth link (the pad powers off on that) and exits |
| `+` / `-` | Light bar brightness up/down in 10% steps (0 turns it off to save battery) |
| `[` / `]` | Volume down/up in steps of 10 (works with any keyboard layout) |
| `s` | Toggles link stats |
| `t` | Toggles a 1 kHz test tone on the pad's speaker |

The keys work in the bridge window. Link stats are printed once a second:

- input report rate and the longest gap between reports;
- delivery delay: how much later than the second's fastest report each one arrived, judged by the pad's own
  timestamps (constant latency isn't visible);
- packets dropped as bad;
- how long writes to the pad (rumble, LED, audio) take.

## Configuration

Settings live in `config.toml` next to `ds4bridge.exe` (template: `bridge/config.toml`).

| Key | Values | What it does |
|---|---|---|
| `brightness` | 0-100 | Light bar brightness at start. `--brightness N` on the command line overrides it |
| `color` | `"#RRGGBB"` | Light bar color until a game sets its own |
| `report_rate` | 250, 500 or 1000 | The pad's Bluetooth input rate in Hz (SDL's values). Unset keeps the pad's default |
| `audio_device` | part of a device name | Output device to play on the pad's speaker (see below) |
| `volume` | 0-100, default 64 | Speaker and headphone volume |

## Playing sound on the pad

Set `audio_device` to part of an output device's name, e.g. `"CABLE Input"`. Whatever plays on that device then
also plays on the pad. The bridge records it with WASAPI loopback, so no extra driver is needed.

- To send a game's sound **only** to the pad, install [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) and
  pick it as the game's output.
- To have the pad play **along** with your speakers, name your speakers.

Audio goes to headphones plugged into the pad's jack, and to the speaker otherwise. If the name is wrong, only
audio stops, and the bridge prints the device names it found.

## How it works

`bridge/ds4bridge.c` reads the Bluetooth pad with hidapi and feeds a ViGEmBus virtual DS4 v2 (`054C:09CC`, USB
layout). Rumble and light bar commands that games write to the virtual pad go back to the real one. Audio is
sent as SBC in output report 0x17 (layout from public captures, confirmed on hardware).

The virtual pad's descriptor and feature reports come from ViGEmBus, not from the real pad. Its gyro/accel
calibration (feature 0x02) belongs to another pad and is fixed in ViGEmBus, so the bridge remaps the real pad's
raw IMU values onto it with SDL's math. Like SDL, it drops Bluetooth packets without the HID flag or, once CRCs
prove reliable, with a bad CRC.

## Known limitations

- No microphone.
- Volume 0-100 is sent to the pad as is; the pad's real range is unknown.
- If the audio device goes away or its format changes, audio stops until the bridge restarts.
- XInput-only games still need ViGEm or Steam Input.

## Tests

| Module | Command |
|---|---|
| `bridge/ds4_translate.c` | `gcc -I bridge bridge/ds4_translate.c test/test_translate.c -o t && ./t` |
| `bridge/config.c` | `gcc -I bridge bridge/config.c test/test_config.c -o tc && ./tc` |
| `bridge/imu.c` | `gcc -I bridge bridge/imu.c test/test_imu.c -lm -o ti && ./ti` |
| `bridge/stats.c` | `gcc -I bridge bridge/stats.c test/test_stats.c -lm -o ts && ./ts` |
| `bridge/sbc.c` | `gcc -I bridge bridge/sbc.c test/test_sbc.c -lm -o tsbc && ./tsbc` |
