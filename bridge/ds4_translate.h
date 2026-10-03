/*
 * DualShock 4: Bluetooth <-> USB report translation.
 * Pure C, no Windows dependencies, so the test builds anywhere.
 *
 * Buffers are HID reports as hidapi returns them: first byte is the report ID,
 * no HIDP transaction header.
 */
#pragma once
#include <stddef.h>
#include <wchar.h>

#define DS4_BT_REPORT_SIZE   78   /* output 0x11 incl. id and CRC32 */
#define DS4_BT_CALIB_SIZE    41   /* feature 0x05 incl. id and CRC32 */
#define DS4_USB_INPUT_SIZE   64   /* input 0x01 incl. id */
#define DS4_USB_OUTPUT_SIZE  32   /* output 0x05 incl. id */
#define DS4_BT_AUDIO_SIZE   462   /* output 0x17 incl. id and CRC32 */
#define DS4_BT_AUDIO_SBC    448   /* 4 SBC frames = 16 ms */

unsigned int ds4_crc32(unsigned int crc, const unsigned char *p, size_t n);

/* BT input 0x11 / short 0x01 -> USB input 0x01.
 * Returns translated length, or 0 if the report is not ours (caller passes it through unchanged). */
size_t ds4_bt_in_to_usb(const unsigned char *in, size_t n, unsigned char *out, size_t cap);

/* USB output 0x05 -> BT output 0x11 with CRC32. interval_ms (0, or 1/2/4 = 1000/500/250 Hz, SDL's values) asks the
 * pad for that input report rate. Returns translated length, or 0 if not ours. */
size_t ds4_usb_out_to_bt(const unsigned char *in, size_t n, unsigned char *out, size_t cap, unsigned char interval_ms);

/* BT output 0x17, audio only (no rumble/LED): 4 SBC frames. frame counts SBC frames sent so far (+4 per report,
 * wraps). Layout from the Habr/SensePost captures: 17 40 A0, frame LE16, target, SBC, CRC32. */
#define DS4_AUDIO_SPEAKER 0x02
#define DS4_AUDIO_HEADSET 0x24
void ds4_bt_audio_report(unsigned short frame, unsigned char target, const unsigned char sbc[DS4_BT_AUDIO_SBC],
                         unsigned char out[DS4_BT_AUDIO_SIZE]);

/* Battery from USB input 0x01 (byte 30), mapped like SDL: percent, or -1 if unknown. *cable = USB cable plugged in. */
int ds4_battery_percent(const unsigned char *usb_in, int *cable);

/* Headphones in the pad's jack, from USB input 0x01 (byte 30, bit 5). */
#define ds4_headphones(usb_in) (((usb_in)[30] & 0x20) != 0)

/* PS (byte 7, bit 0) and Triangle (byte 5, bit 7) both held, from USB input 0x01: Steam's "turn the pad off". */
#define ds4_power_off_combo(usb_in) (((usb_in)[7] & 0x01) && ((usb_in)[5] & 0x80))

/* Bluetooth address from the HID serial Windows reports for a Bluetooth pad: 12 hex digits, ':' or '-' allowed
 * between them. Returns 1 and sets *addr (as BTH_ADDR, first byte most significant), or 0. */
int ds4_parse_mac(const wchar_t *s, unsigned long long *addr);

/* Light bar dimming for USB output 0x05: remembers the color the report sets (flag 0x02) in led[3],
 * then makes the report set the light bar to led scaled to percent (0 = off). */
void ds4_usb_out_dim_led(unsigned char *usb_out, unsigned char led[3], int percent);

/* BT input check, as SDL does it: a 0x11 report needs the HID-data flag, and once CRCs have been coming in valid,
 * one with a bad CRC is dropped. *good_crcs is the caller's running count (start at 0). Short 0x01 reports pass. */
int ds4_bt_in_valid(const unsigned char *in, size_t n, unsigned *good_crcs);
