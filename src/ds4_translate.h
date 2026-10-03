/*
 * DualShock 4: Bluetooth <-> USB report translation.
 * Pure C, no kernel/WDF dependencies, so it builds both in the driver and in the test.
 *
 * Buffers are HID reports as HidBth exchanges them: first byte is the report ID,
 * no HIDP transaction header.
 */
#pragma once
#include <stddef.h>

#define DS4_BT_REPORT_SIZE   78   /* output 0x11 incl. id and CRC32 */
#define DS4_BT_CALIB_SIZE    41   /* feature 0x05 incl. id and CRC32 */
#define DS4_USB_INPUT_SIZE   64   /* input 0x01 incl. id */
#define DS4_USB_OUTPUT_SIZE  32   /* output 0x05 incl. id */
#define DS4_USB_CALIB_SIZE   37   /* feature 0x02 incl. id */

unsigned int ds4_crc32(unsigned int crc, const unsigned char *p, size_t n);

/* BT input 0x11 / short 0x01, or BT feature 0x05 (calibration) -> USB input 0x01 / feature 0x02.
 * Returns translated length, or 0 if the report is not ours (caller passes it through unchanged). */
size_t ds4_bt_in_to_usb(const unsigned char *in, size_t n, unsigned char *out, size_t cap);

/* USB output 0x05 -> BT output 0x11 with CRC32. Returns translated length, or 0 if not ours. */
size_t ds4_usb_out_to_bt(const unsigned char *in, size_t n, unsigned char *out, size_t cap);

/* Battery from USB input 0x01 (byte 30), mapped like SDL: percent, or -1 if unknown. *cable = USB cable plugged in. */
int ds4_battery_percent(const unsigned char *usb_in, int *cable);

/* Light bar dimming for USB output 0x05: remembers the color the report sets (flag 0x02) in led[3],
 * then makes the report set the light bar to led scaled to percent (0 = off). */
void ds4_usb_out_dim_led(unsigned char *usb_out, unsigned char led[3], int percent);
