/*
 * ds4bridge: user-mode alternative to ds4bt.sys. No test signing needed.
 * Reads the Bluetooth DS4 with hidapi and mirrors it onto a ViGEmBus virtual DS4 v2 (USB layout).
 * Output reports written to the virtual pad (rumble, light bar) go back to the real one over Bluetooth.
 * Hide the real pad from games with HidHide; the bridge prints the commands at startup.
 */
#include <windows.h>
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <string.h>

#include <hidapi.h>
#include <ViGEm/Client.h>
#include "ds4_translate.h"

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

/* HidHide hides by device instance ID, so print the commands that hide this pad. */
static void print_hidhide_setup(const char *path)
{
    WCHAR wpath[512], inst[512], exe[MAX_PATH];
    ULONG size = sizeof inst;
    DEVPROPTYPE type;

    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, ARRAYSIZE(wpath));
    if (CM_Get_Device_Interface_PropertyW(wpath, &DEVPKEY_Device_InstanceId, &type,
                                          (PBYTE)inst, &size, 0) != CR_SUCCESS)
        return;
    GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    wprintf(L"To hide the real pad from games (once, elevated prompt, HidHideCLI.exe from HidHide's install folder):\n"
            L"  HidHideCLI.exe --app-reg \"%ls\" --dev-hide \"%ls\" --cloak-on\n\n", exe, inst);
}

static DWORD WINAPI output_thread(LPVOID unused)
{
    (void)unused;
    DS4_OUTPUT_BUFFER usb;
    unsigned char bt[DS4_BT_REPORT_SIZE];

    for (;;) {
        VIGEM_ERROR err = vigem_target_ds4_await_output_report(vigem, vpad, &usb);
        if (!VIGEM_SUCCESS(err)) {
            fprintf(stderr, "ViGEm output wait failed: 0x%08X\n", err);
            ExitProcess(1);
        }
        size_t n = ds4_usb_out_to_bt(usb.Buffer, sizeof usb.Buffer, bt, sizeof bt);
        if (n && hid_write(pad, bt, n) < 0)
            fwprintf(stderr, L"write to pad failed: %ls\n", hid_error(pad));
    }
}

int main(void)
{
    if (hid_init())
        return fprintf(stderr, "hid_init failed\n"), 1;

    char *path = find_pad();
    if (!path)
        return fprintf(stderr, "No Bluetooth DualShock 4 found (pair it first; if HidHide hides it, whitelist this exe).\n"), 1;
    pad = hid_open_path(path);
    if (!pad)
        return fprintf(stderr, "Can't open %s\n", path), 1;
    print_hidhide_setup(path);

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

    CreateThread(NULL, 0, output_thread, NULL, 0, NULL);
    printf("Bridging. Ctrl+C to stop (the virtual pad goes away with the process).\n");

    /* ponytail: exits when the pad disconnects; wrap in a reconnect loop if that gets annoying. */
    unsigned char in[128], usb[DS4_USB_INPUT_SIZE];
    DS4_REPORT_EX report;
    for (;;) {
        int n = hid_read(pad, in, sizeof in);
        if (n < 0)
            return fwprintf(stderr, L"pad read failed (disconnected?): %ls\n", hid_error(pad)), 1;
        if (ds4_bt_in_to_usb(in, (size_t)n, usb, sizeof usb) != DS4_USB_INPUT_SIZE || usb[0] != 0x01)
            continue;
        memcpy(report.ReportBuffer, usb + 1, sizeof report.ReportBuffer);
        vigem_target_ds4_update_ex(vigem, vpad, report);
    }
}
