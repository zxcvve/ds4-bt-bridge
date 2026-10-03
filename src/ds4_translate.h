/*
 * DualShock 4: Bluetooth <-> USB report translation.
 * Pure C, no kernel/WDF dependencies, so it builds both in the driver and in the test.
 *
 * All buffers are L2CAP HID payloads, i.e. they start with the HIDP transaction byte:
 *   0xA1 DATA|Input, 0xA2 DATA|Output, 0xA3 DATA|Feature, 0x52 SET_REPORT|Output, 0x43 GET_REPORT|Feature.
 */
#pragma once
#include <stddef.h>

#define DS4_BT_REPORT_SIZE   78   /* report 0x11 incl. id and CRC32 */
#define DS4_USB_INPUT_SIZE   64   /* report 0x01 incl. id */
#define DS4_USB_OUTPUT_SIZE  32   /* report 0x05 incl. id */
#define DS4_USB_CALIB_SIZE   37   /* feature 0x02 incl. id */

unsigned int ds4_crc32(unsigned int crc, const unsigned char *p, size_t n);

/* BT input (A1 11 / A1 01 short / A3 05 calibration) -> USB form (A1 01 / A3 02).
 * Returns translated length, or 0 if the payload is not ours (caller passes it through unchanged). */
size_t ds4_bt_in_to_usb(const unsigned char *in, size_t n, unsigned char *out, size_t cap);

/* USB output (A2 05 / 52 05) -> BT output (A2 11 / 52 11) with CRC32.
 * Returns translated length, or 0 if not ours. */
size_t ds4_usb_out_to_bt(const unsigned char *in, size_t n, unsigned char *out, size_t cap);

/* GET_REPORT for USB calibration feature 0x02 -> BT feature 0x05. In place, same length. */
void ds4_fix_get_feature(unsigned char *p, size_t n);
