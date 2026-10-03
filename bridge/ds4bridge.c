/*
 * ds4bridge: user-mode alternative to ds4bt.sys. No test signing needed.
 * Reads the Bluetooth DS4 with hidapi and mirrors it onto a ViGEmBus virtual DS4 v2 (USB layout).
 * Output reports written to the virtual pad (rumble, light bar) go back to the real one over Bluetooth.
 * While it runs, the real pad is hidden from other apps with HidHide (if installed).
 */
#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <hidapi.h>
#include <ViGEm/Client.h>
#include "ds4_translate.h"
#include "config.h"

#define SONY_VID     0x054C
#define DS4_V1_PID   0x05C4
#define DS4_V2_PID   0x09CC

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
static int battery = -1, cable;

static void update_title(void)
{
    WCHAR title[128];
    if (battery < 0)
        swprintf(title, ARRAYSIZE(title), L"ds4bridge - battery ? - light bar %d%%", brightness);
    else
        swprintf(title, ARRAYSIZE(title), L"ds4bridge - battery %d%%%ls - light bar %d%%",
                 battery, cable ? (battery == 100 ? L" (full)" : L" (charging)") : L"", brightness);
    SetConsoleTitleW(title);
}

/* Sends a USB output 0x05 to the pad, light bar dimmed to the user's brightness. Called from two threads. */
static void send_to_pad(unsigned char *usb)
{
    unsigned char bt[DS4_BT_REPORT_SIZE];
    EnterCriticalSection(&out_lock);
    ds4_usb_out_dim_led(usb, led, brightness);
    size_t n = ds4_usb_out_to_bt(usb, DS4_USB_OUTPUT_SIZE, bt, sizeof bt);
    if (n && hid_write(pad, bt, n) < 0)
        fwprintf(stderr, L"write to pad failed: %ls\n", hid_error(pad));
    LeaveCriticalSection(&out_lock);
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
        send_to_pad(usb);
    }
}

static DWORD WINAPI keyboard_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        int key = _getch();
        if (key == '+' || key == '=')
            set_brightness(brightness + 10);
        else if (key == '-' || key == '_')
            set_brightness(brightness - 10);
    }
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
    struct bridge_config cfg = { 100, { 0x00, 0x00, 0x40 } };     /* SDL's player-1 blue */
    load_config(&cfg);
    memcpy(led, cfg.color, sizeof led);
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
    if (hid_get_feature_report(pad, calib, sizeof calib) < 0)
        fwprintf(stderr, L"calibration read failed (no gyro/touchpad): %ls\n", hid_error(pad));

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
    printf("Bridging. +/- changes light bar brightness; battery is in the window title. Ctrl+C to stop.\n");

    /* ponytail: exits when the pad disconnects; wrap in a reconnect loop if that gets annoying. */
    unsigned char in[128], usb[DS4_USB_INPUT_SIZE], last_id = 0;
    DS4_REPORT_EX report;
    DWORD last_input = GetTickCount(), last_poke = 0;
    BOOL stalled = FALSE;
    for (;;) {
        int n = hid_read_timeout(pad, in, sizeof in, 100);
        if (n < 0)
            return fwprintf(stderr, L"pad read failed (disconnected?): %ls\n", hid_error(pad)), 1;
        DWORD now = GetTickCount();
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
        if (ds4_bt_in_to_usb(in, (size_t)n, usb, sizeof usb) != DS4_USB_INPUT_SIZE || usb[0] != 0x01)
            continue;
        memcpy(report.ReportBuffer, usb + 1, sizeof report.ReportBuffer);
        vigem_target_ds4_update_ex(vigem, vpad, report);

        int c = cable, pct = in[0] == 0x11 ? ds4_battery_percent(usb, &c) : battery;   /* short reports have no battery */
        if (pct != battery || (pct >= 0 && c != cable)) {
            battery = pct;
            cable = c;
            update_title();
        }
    }
}
