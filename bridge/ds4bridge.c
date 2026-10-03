/*
 * ds4bridge: makes a Bluetooth DS4 look like a USB one to games. No test signing needed.
 * Reads the Bluetooth DS4 with hidapi and mirrors it onto a ViGEmBus virtual DS4 v2 (USB layout).
 * Output reports written to the virtual pad (rumble, light bar) go back to the real one over Bluetooth.
 * While it runs, the real pad is hidden from other apps with HidHide (if installed).
 */
#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <hidapi.h>
#include <ViGEm/Client.h>
#include "ds4_translate.h"
#include "config.h"
#include "imu.h"
#include "stats.h"
#include "sbc.h"
#include "audio.h"

#define SONY_VID     0x054C
#define DS4_V1_PID   0x05C4
#define DS4_V2_PID   0x09CC

/* What ViGEmBus answers for feature 0x02 on every virtual DS4 (sys/Ds4Pdo.cpp): another pad's calibration.
 * Games decode the virtual pad's gyro/accel with it, so the bridge remaps the real pad's values onto it. */
static const unsigned char vigem_calib[] = {
    0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x87, 0x22, 0x7B, 0xDD, 0xB2, 0x22, 0x47, 0xDD, 0xBD,
    0x22, 0x43, 0xDD, 0x1C, 0x02, 0x1C, 0x02, 0x7F, 0x1E, 0x2E, 0xDF, 0x60, 0x1F, 0x4C, 0xE0, 0x3A,
    0x1D, 0xC6, 0xDE, 0x08, 0x00
};

static hid_device *pad;
static PVIGEM_CLIENT vigem;
static PVIGEM_TARGET vpad;

/* Bluetooth only: the virtual pad is also 054C:09CC and must not be picked up. */
static char *find_pad(void)
{
    char *path = NULL;
    struct hid_device_info *list = hid_enumerate(SONY_VID, 0);
    for (struct hid_device_info *d = list; d; d = d->next) {
        if (d->bus_type == HID_API_BUS_BLUETOOTH &&
            (d->product_id == DS4_V1_PID || d->product_id == DS4_V2_PID)) {
            path = _strdup(d->path);
            break;
        }
    }
    hid_free_enumeration(list);
    return path;
}

/* HidHide control device (Shared/HidHideIoctlContract.h); open to every user, no elevation needed. */
#define HH_IOCTL(fn)            CTL_CODE(32769, fn, METHOD_BUFFERED, FILE_READ_DATA)
#define HH_GET_WHITELIST        HH_IOCTL(2048)
#define HH_SET_WHITELIST        HH_IOCTL(2049)
#define HH_GET_BLACKLIST        HH_IOCTL(2050)
#define HH_SET_BLACKLIST        HH_IOCTL(2051)
#define HH_GET_ACTIVE           HH_IOCTL(2052)
#define HH_SET_ACTIVE           HH_IOCTL(2053)

static HANDLE hidhide = INVALID_HANDLE_VALUE;
static WCHAR hidden_inst[512];
static BOOLEAN was_active;
static volatile LONG is_hidden;

/* Adds or removes one entry of a HidHide MULTI_SZ list (case-insensitive, duplicates dropped). */
static BOOL hh_edit(DWORD get, DWORD set, const WCHAR *item, BOOL add)
{
    DWORD needed = 0;
    if (!DeviceIoControl(hidhide, get, NULL, 0, NULL, 0, &needed, NULL))
        return FALSE;
    size_t extra = (wcslen(item) + 2) * sizeof(WCHAR);
    WCHAR *list = calloc(1, needed + 2 * sizeof(WCHAR)), *out = calloc(1, needed + extra), *o = out;
    BOOL ok = list && out && DeviceIoControl(hidhide, get, NULL, 0, list, needed, &needed, NULL);
    if (ok) {
        for (WCHAR *p = list; *p; p += wcslen(p) + 1)
            if (_wcsicmp(p, item)) {
                wcscpy(o, p);
                o += wcslen(p) + 1;
            }
        if (add) {
            wcscpy(o, item);
            o += wcslen(item) + 1;
        }
        *o++ = L'\0';
        ok = DeviceIoControl(hidhide, set, out, (DWORD)((o - out) * sizeof(WCHAR)), NULL, 0, &needed, NULL);
    }
    free(list);
    free(out);
    return ok;
}

static BOOL hh_set_active(BOOLEAN on)
{
    DWORD needed;
    return DeviceIoControl(hidhide, HH_SET_ACTIVE, &on, sizeof on, NULL, 0, &needed, NULL);
}

/* Runs at exit, on Ctrl+C and on console close. */
static void unhide_pad(void)
{
    if (!InterlockedExchange(&is_hidden, 0))
        return;
    if (!hh_edit(HH_GET_BLACKLIST, HH_SET_BLACKLIST, hidden_inst, FALSE))
        fwprintf(stderr, L"HidHide: couldn't unhide %ls (error %lu)\n", hidden_inst, GetLastError());
    if (!was_active)
        hh_set_active(FALSE);
}

static BOOL WINAPI on_console_ctrl(DWORD type)
{
    (void)type;
    unhide_pad();
    return FALSE;   /* let the default handler end the process */
}

/* The pad is already open, and HidHide only blocks new opens, so the bridge keeps reading it.
 * ponytail: a killed or crashed bridge leaves the pad hidden until its next clean exit; HidHide master has a
 * process-lifetime IOCTL_ADD_SESSION_BLACKLIST that fixes this, switch to it once a release ships it. */
static void hide_pad(const char *path)
{
    WCHAR wpath[512], exe_dos[MAX_PATH], exe[MAX_PATH];
    ULONG size = sizeof hidden_inst;
    DEVPROPTYPE type;
    DWORD needed;

    hidhide = CreateFileW(L"\\\\.\\HidHide", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, OPEN_EXISTING, 0, NULL);
    if (hidhide == INVALID_HANDLE_VALUE) {
        printf("HidHide not installed: games will see both the real and the virtual pad.\n");
        return;
    }

    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, ARRAYSIZE(wpath));
    if (CM_Get_Device_Interface_PropertyW(wpath, &DEVPKEY_Device_InstanceId, &type,
                                          (PBYTE)hidden_inst, &size, 0) != CR_SUCCESS) {
        fprintf(stderr, "HidHide: can't get the pad's instance ID; not hiding it.\n");
        return;
    }

    /* Whitelist ourselves (HidHide wants the NT path) so a restart still finds a pad left hidden by a crash. */
    GetModuleFileNameW(NULL, exe_dos, ARRAYSIZE(exe_dos));
    HANDLE self = CreateFileW(exe_dos, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    BOOL ok = self != INVALID_HANDLE_VALUE &&
              GetFinalPathNameByHandleW(self, exe, ARRAYSIZE(exe), VOLUME_NAME_NT) &&
              hh_edit(HH_GET_WHITELIST, HH_SET_WHITELIST, exe, TRUE);
    if (self != INVALID_HANDLE_VALUE)
        CloseHandle(self);

    ok = ok && DeviceIoControl(hidhide, HH_GET_ACTIVE, NULL, 0, &was_active, sizeof was_active, &needed, NULL) &&
         hh_edit(HH_GET_BLACKLIST, HH_SET_BLACKLIST, hidden_inst, TRUE);
    if (!ok) {
        fwprintf(stderr, L"HidHide: couldn't hide the pad (error %lu)\n", GetLastError());
        return;
    }
    is_hidden = 1;
    atexit(unhide_pad);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    if (!hh_set_active(TRUE))
        fwprintf(stderr, L"HidHide: couldn't enable hiding (error %lu)\n", GetLastError());
    wprintf(L"HidHide: real pad hidden while the bridge runs (%ls)\n", hidden_inst);
}

/* Light bar color last set by a game (config color until then) and the user's brightness. */
static CRITICAL_SECTION out_lock;
static unsigned char led[3];
static int brightness = 100;
static int volume = 64;                 /* speaker and headphones, 0-100; guarded by out_lock */
static unsigned char report_interval;   /* from config.toml; 0 = pad's default */
static int battery = -1, cable;

static void update_title(void)
{
    WCHAR title[128];
    if (battery < 0)
        swprintf(title, ARRAYSIZE(title), L"ds4bridge - battery ? - light bar %d%% - volume %d%%", brightness, volume);
    else
        swprintf(title, ARRAYSIZE(title), L"ds4bridge - battery %d%%%ls - light bar %d%% - volume %d%%",
                 battery, cable ? (battery == 100 ? L" (full)" : L" (charging)") : L"", brightness, volume);
    SetConsoleTitleW(title);
}

/* Sends a USB output 0x05 to the pad, light bar dimmed to the user's brightness. Called from two threads. */
static uint64_t now_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / freq.QuadPart * 1000000 + t.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart);
}

/* Link statistics, printed once a second while 's' has them on. Output figures are guarded by out_lock. */
static struct link_stats lstats;
static volatile LONG show_stats;
static unsigned out_writes;
static uint64_t out_sum_us, out_max_us;

/* Caller holds out_lock. */
static void write_pad(const unsigned char *bt, size_t n)
{
    uint64_t t0 = now_us();
    int r = hid_write(pad, bt, n);
    uint64_t dt = now_us() - t0;
    out_writes++;
    out_sum_us += dt;
    if (dt > out_max_us)
        out_max_us = dt;
    if (r < 0)
        fwprintf(stderr, L"write to pad failed: %ls\n", hid_error(pad));
}

static void send_to_pad(unsigned char *usb)
{
    unsigned char bt[DS4_BT_REPORT_SIZE];
    EnterCriticalSection(&out_lock);
    ds4_usb_out_dim_led(usb, led, brightness);
    size_t n = ds4_usb_out_to_bt(usb, DS4_USB_OUTPUT_SIZE, bt, sizeof bt, report_interval);
    if (n)
        write_pad(bt, n);
    LeaveCriticalSection(&out_lock);
}

/* Speaker: 16 ms chunks of 32 kHz stereo, each sent as one 0x17 report of 4 SBC frames. */
#define AUDIO_CHUNK (4 * SBC_FRAME_SAMPLES)
static volatile LONG tone_on;   /* 't': 1 kHz test tone, replaces captured audio while on */
static char audio_device[128];  /* from config.toml; "" = no capture */
static volatile LONG headphones;    /* plugged into the pad: audio goes to the jack instead of the speaker */

/* Volume: headset L/R, mic, speaker (flags 0x10-0x80) at USB[19..22]; USB[23] = 0x85 as in the Habr capture.
 * ponytail: the pad's real range is unknown; 0-100 is sent as is (the Habr capture calls it a percentage). */
static void set_speaker_volume(void)
{
    unsigned char vol[DS4_USB_OUTPUT_SIZE] = { 0x05, 0xF0, 0x04 };
    EnterCriticalSection(&out_lock);
    vol[19] = vol[20] = vol[22] = (unsigned char)volume;
    LeaveCriticalSection(&out_lock);
    vol[23] = 0x85;
    send_to_pad(vol);
}

static void set_volume(int percent)
{
    EnterCriticalSection(&out_lock);
    volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    LeaveCriticalSection(&out_lock);
    set_speaker_volume();
    update_title();
}

/* Called from the tone and capture threads; out_lock also guards the encoder and frame counter. */
static void send_audio(const short *pcm)
{
    static struct sbc_enc enc;
    static unsigned short frame;
    unsigned char sbc[DS4_BT_AUDIO_SBC], bt[DS4_BT_AUDIO_SIZE];
    EnterCriticalSection(&out_lock);
    for (int f = 0; f < 4; f++)
        sbc_encode(&enc, pcm + f * 2 * SBC_FRAME_SAMPLES, sbc + f * SBC_FRAME_SIZE);
    ds4_bt_audio_report(frame, headphones ? DS4_AUDIO_HEADSET : DS4_AUDIO_SPEAKER, sbc, bt);
    frame += 4;
    write_pad(bt, sizeof bt);
    LeaveCriticalSection(&out_lock);
}

static DWORD WINAPI tone_thread(LPVOID unused)
{
    (void)unused;
    short pcm[2 * AUDIO_CHUNK];
    int n = 0;

    for (;;) {
        while (!tone_on)
            Sleep(50);
        set_speaker_volume();
        uint64_t start = now_us(), queued_us = 0;
        while (tone_on) {
            /* Stay at most 32 ms ahead of real time; Sleep's default 15.6 ms granularity fits in that. */
            if (queued_us > now_us() - start + 32000) {
                Sleep(4);
                continue;
            }
            for (int i = 0; i < AUDIO_CHUNK; i++, n = (n + 1) % 32)     /* 32 samples = one 1 kHz period */
                pcm[2 * i] = pcm[2 * i + 1] = (short)(8000 * sin(6.283185307179586 * n / 32));
            send_audio(pcm);
            queued_us += 16000;
        }
    }
}

/* Captured audio arrives in real time, so it needs no pacing of its own. */
static void captured(const short *pcm)
{
    if (!tone_on)
        send_audio(pcm);
}

static DWORD WINAPI capture_thread(LPVOID unused)
{
    (void)unused;
    set_speaker_volume();
    audio_capture(audio_device, AUDIO_CHUNK, captured);
    return 0;
}

/* Starts a new statistics window; prints the finished one if stats are on. */
static void print_stats(DWORD elapsed_ms)
{
    struct link_window w;
    stats_take(&lstats, &w);
    EnterCriticalSection(&out_lock);
    unsigned writes = out_writes;
    uint64_t sum = out_sum_us, max = out_max_us;
    out_writes = 0;
    out_sum_us = out_max_us = 0;
    LeaveCriticalSection(&out_lock);
    if (!show_stats)
        return;
    printf("input %.0f Hz, max gap %.1f ms, delivery delay avg %.2f / max %.2f ms, %u bad"
           " | output %u writes, avg %.2f / max %.2f ms\n",
           w.reports * 1000.0 / elapsed_ms, w.max_gap_ms, w.delay_avg_ms, w.delay_max_ms, w.bad,
           writes, writes ? sum / 1000.0 / writes : 0.0, max / 1000.0);
}

/* Re-sends only the light bar (no rumble flag, so the motors keep their state). */
static void set_brightness(int percent)
{
    unsigned char usb[DS4_USB_OUTPUT_SIZE] = { 0x05, 0x00, 0x04 };
    EnterCriticalSection(&out_lock);
    brightness = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    LeaveCriticalSection(&out_lock);
    send_to_pad(usb);
    update_title();
}

static DWORD WINAPI output_thread(LPVOID unused)
{
    (void)unused;
    DS4_OUTPUT_BUFFER out;
    unsigned char usb[DS4_USB_OUTPUT_SIZE];

    for (;;) {
        VIGEM_ERROR err = vigem_target_ds4_await_output_report(vigem, vpad, &out);
        if (!VIGEM_SUCCESS(err)) {
            fprintf(stderr, "ViGEm output wait failed: 0x%08X\n", err);
            exit(1);    /* not ExitProcess: atexit has to unhide the pad */
        }
        if (out.Buffer[0] != 0x05)
            continue;
        memcpy(usb, out.Buffer, sizeof usb);
        usb[1] &= 0x0F;     /* the bridge owns the volumes: a game's zeros would mute the speaker */
        send_to_pad(usb);
    }
}

/* ReadConsoleInput, not _getch: _getch puts the console in raw mode while it waits, which turns Ctrl+C into a
 * plain key instead of the signal that unhides the pad and exits. */
static DWORD WINAPI keyboard_thread(LPVOID unused)
{
    (void)unused;
    HANDLE con = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD rec;
    DWORD got;
    while (ReadConsoleInputW(con, &rec, 1, &got)) {     /* fails if stdin isn't a console: no keys then */
        if (got != 1 || rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
            continue;
        WCHAR key = rec.Event.KeyEvent.uChar.UnicodeChar;
        if (key == '+' || key == '=')
            set_brightness(brightness + 10);
        else if (key == '-' || key == '_')
            set_brightness(brightness - 10);
        else if (key == 's' || key == 'S')
            printf("Stats %s.\n", InterlockedXor(&show_stats, 1) ? "off" : "on");
        else if (key == ']' || key == '}')
            set_volume(volume + 10);
        else if (key == '[' || key == '{')
            set_volume(volume - 10);
        else if (key == 't' || key == 'T')
            printf("Test tone %s.\n", InterlockedXor(&tone_on, 1) ? "off" : "on");
    }
    return 0;
}

/* config.toml next to the exe; missing file means defaults, a bad one stops the bridge. */
static void load_config(struct bridge_config *cfg)
{
    static char text[16384];
    WCHAR path[MAX_PATH];
    const char *err;

    DWORD len = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    WCHAR *slash = wcsrchr(path, L'\\');
    if (!len || !slash || (size_t)(slash - path) + 13 > ARRAYSIZE(path))
        return;
    wcscpy(slash + 1, L"config.toml");
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    int line = config_parse(text, cfg, &err);
    if (line) {
        fwprintf(stderr, L"%ls, line %d: %hs\n", path, line, err);
        exit(2);
    }
}

int main(int argc, char **argv)
{
    struct bridge_config cfg = { 100, { 0x00, 0x00, 0x40 }, 0, 64, "" };  /* SDL's player-1 blue */
    load_config(&cfg);
    memcpy(led, cfg.color, sizeof led);
    report_interval = (unsigned char)cfg.report_interval_ms;      /* sent with the first light bar report */
    strcpy(audio_device, cfg.audio_device);
    volume = cfg.volume;
    int start_brightness = cfg.brightness;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--brightness") && i + 1 < argc)
            start_brightness = atoi(argv[++i]);
        else
            return fprintf(stderr, "usage: ds4bridge [--brightness 0-100]\n"), 2;
    }

    if (hid_init())
        return fprintf(stderr, "hid_init failed\n"), 1;

    char *path = find_pad();
    if (!path)
        return fprintf(stderr, "No Bluetooth DualShock 4 found (pair it first; if HidHide hides it, whitelist this exe).\n"), 1;
    pad = hid_open_path(path);
    if (!pad)
        return fprintf(stderr, "Can't open %s\n", path), 1;

    /* Reading calibration switches the pad from the short 0x01 to the full 0x11 input report. */
    unsigned char calib[DS4_BT_CALIB_SIZE] = { 0x05 };
    int calib_n = hid_get_feature_report(pad, calib, sizeof calib);
    if (calib_n < 0)
        fwprintf(stderr, L"calibration read failed (no gyro/touchpad): %ls\n", hid_error(pad));
    struct ds4_imu_cal pad_cal, vigem_cal;
    if (!ds4_imu_cal_parse(calib, calib_n < 0 ? 0 : (size_t)calib_n, 1, &pad_cal))
        printf("Pad calibration missing or implausible; gyro/accel use SDL's defaults.\n");
    ds4_imu_cal_parse(vigem_calib, sizeof vigem_calib, 0, &vigem_cal);

    vigem = vigem_alloc();
    VIGEM_ERROR err = vigem_connect(vigem);
    if (!VIGEM_SUCCESS(err))
        return fprintf(stderr, "ViGEmBus not available (installed?): 0x%08X\n", err), 1;
    vpad = vigem_target_ds4_alloc();
    vigem_target_set_vid(vpad, SONY_VID);
    vigem_target_set_pid(vpad, DS4_V2_PID);
    err = vigem_target_add(vigem, vpad);
    if (!VIGEM_SUCCESS(err))
        return fprintf(stderr, "Can't add virtual DS4: 0x%08X\n", err), 1;
    hide_pad(path);

    InitializeCriticalSection(&out_lock);
    set_brightness(start_brightness);
    CreateThread(NULL, 0, output_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, keyboard_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, tone_thread, NULL, 0, NULL);
    if (audio_device[0])
        CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    printf("Bridging. +/- changes light bar brightness, [/] the volume, s toggles link stats, t plays a test tone;"
           " battery is in the window title.\n"
           "Ctrl+C to stop.\n");

    /* ponytail: exits when the pad disconnects; wrap in a reconnect loop if that gets annoying. */
    unsigned char in[128], usb[DS4_USB_INPUT_SIZE], last_id = 0;
    DS4_REPORT_EX report;
    DWORD last_input = GetTickCount(), last_poke = 0, last_stats = GetTickCount();
    unsigned good_crcs = 0;
    BOOL stalled = FALSE;
    for (;;) {
        int n = hid_read_timeout(pad, in, sizeof in, 100);
        uint64_t arrival_us = now_us();
        if (n < 0)
            return fwprintf(stderr, L"pad read failed (disconnected?): %ls\n", hid_error(pad)), 1;
        DWORD now = GetTickCount();
        if (now - last_stats >= 1000) {
            print_stats(now - last_stats);
            last_stats = now;
        }
        if (n == 0) {
            /* In 0x11 mode the pad streams nonstop, so silence means the link stalled; Windows may not deliver
             * input again until something is written to the pad. SDL pokes after 500 ms the same way. */
            if (last_id == 0x11 && now - last_input >= 500 && now - last_poke >= 500) {
                if (!stalled)
                    printf("No input for %lu ms; poking the pad.\n", (unsigned long)(now - last_input));
                stalled = TRUE;
                unsigned char poke[DS4_USB_OUTPUT_SIZE] = { 0x05, 0x00, 0x04 };    /* light bar only, unchanged */
                send_to_pad(poke);
                last_poke = now;
            }
            continue;
        }
        if (stalled)
            printf("Input resumed after %lu ms.\n", (unsigned long)(now - last_input));
        if (last_id == 0x11 && in[0] != 0x11)
            printf("Pad switched from extended to 0x%02X reports.\n", in[0]);
        stalled = FALSE;
        last_input = now;
        last_id = in[0];
        if (!ds4_bt_in_valid(in, (size_t)n, &good_crcs)) {
            stats_bad(&lstats);
            continue;
        }
        if (ds4_bt_in_to_usb(in, (size_t)n, usb, sizeof usb) != DS4_USB_INPUT_SIZE || usb[0] != 0x01)
            continue;
        if (in[0] == 0x11) {
            stats_input(&lstats, arrival_us, (uint16_t)(usb[10] | usb[11] << 8));    /* pad timestamp */
            ds4_imu_remap(usb, &pad_cal, &vigem_cal);
        }
        memcpy(report.ReportBuffer, usb + 1, sizeof report.ReportBuffer);
        vigem_target_ds4_update_ex(vigem, vpad, report);

        int c = cable, pct = in[0] == 0x11 ? ds4_battery_percent(usb, &c) : battery;   /* short reports have no battery */
        if (pct != battery || (pct >= 0 && c != cable)) {
            battery = pct;
            cable = c;
            update_title();
        }
        if (in[0] == 0x11 && ds4_headphones(usb) != headphones) {
            headphones = ds4_headphones(usb);
            printf("Headphones %s: audio to the %s.\n", headphones ? "plugged in" : "unplugged", headphones ? "jack" : "speaker");
        }
    }
}
