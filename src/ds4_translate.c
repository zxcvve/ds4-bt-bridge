#include <string.h>
#include "ds4_translate.h"

/* Standard CRC32 (zlib). DS4 BT reports carry crc32(transaction byte + report[0..73]). */
unsigned int ds4_crc32(unsigned int crc, const unsigned char *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

/* ponytail: input CRCs are not verified; the baseband already checks ACL packets,
 * and a corrupt frame only glitches one sample. Verify here if that ever shows up. */
size_t ds4_bt_in_to_usb(const unsigned char *in, size_t n, unsigned char *out, size_t cap)
{
    if (n < 2)
        return 0;

    if (in[0] == 0xA1 && in[1] == 0x11) {
        /* Extended report: BT[3..65] == USB[1..63]. */
        if (n < 1 + 3 + DS4_USB_INPUT_SIZE - 1 || cap < 1 + DS4_USB_INPUT_SIZE)
            return 0;
        out[0] = 0xA1;
        out[1] = 0x01;
        memcpy(out + 2, in + 1 + 3, DS4_USB_INPUT_SIZE - 1);
        return 1 + DS4_USB_INPUT_SIZE;
    }

    if (in[0] == 0xA1 && in[1] == 0x01) {
        /* Short report (before extended mode kicks in): sticks/buttons/triggers share the USB layout. */
        if (cap < 1 + DS4_USB_INPUT_SIZE)
            return 0;
        size_t copy = n - 1 < DS4_USB_INPUT_SIZE ? n - 1 : DS4_USB_INPUT_SIZE;
        memset(out, 0, 1 + DS4_USB_INPUT_SIZE);
        memcpy(out, in, 1 + copy);
        return 1 + DS4_USB_INPUT_SIZE;
    }

    if (in[0] == 0xA3 && in[1] == 0x05) {
        /* Calibration: BT 0x05 is USB 0x02 plus CRC; data bytes are identical. */
        if (n < 1 + DS4_USB_CALIB_SIZE || cap < 1 + DS4_USB_CALIB_SIZE)
            return 0;
        out[0] = 0xA3;
        out[1] = 0x02;
        memcpy(out + 2, in + 2, DS4_USB_CALIB_SIZE - 1);
        return 1 + DS4_USB_CALIB_SIZE;
    }

    return 0;
}

size_t ds4_usb_out_to_bt(const unsigned char *in, size_t n, unsigned char *out, size_t cap)
{
    if (n < 2 || (in[0] != 0xA2 && in[0] != 0x52) || in[1] != 0x05 || cap < 1 + DS4_BT_REPORT_SIZE)
        return 0;

    unsigned char *r = out + 1;
    memset(out, 0, 1 + DS4_BT_REPORT_SIZE);
    out[0] = in[0];
    r[0] = 0x11;
    r[1] = 0xC0;                         /* HID + CRC present, poll interval 0 */
    /* USB[1..31] (flags, 0x04, pad, rumble R/L, RGB, flash on/off, ...) == BT[3..33] */
    size_t copy = n - 2 < DS4_USB_OUTPUT_SIZE - 1 ? n - 2 : DS4_USB_OUTPUT_SIZE - 1;
    memcpy(r + 3, in + 2, copy);

    /* CRC seed is always 0xA2 for output, including SET_REPORT on the control channel. */
    unsigned char seed = 0xA2;
    unsigned int crc = ds4_crc32(0, &seed, 1);
    crc = ds4_crc32(crc, r, DS4_BT_REPORT_SIZE - 4);
    r[74] = (unsigned char)crc;
    r[75] = (unsigned char)(crc >> 8);
    r[76] = (unsigned char)(crc >> 16);
    r[77] = (unsigned char)(crc >> 24);
    return 1 + DS4_BT_REPORT_SIZE;
}

void ds4_fix_get_feature(unsigned char *p, size_t n)
{
    /* 0x43 = GET_REPORT|Feature; 0x4B additionally carries a 2-byte max size. */
    if (n < 2 || (p[0] & 0xF7) != 0x43 || p[1] != 0x02)
        return;
    p[1] = 0x05;
    if ((p[0] & 0x08) && n >= 4) {
        unsigned int sz = p[2] | (p[3] << 8);
        if (sz < 41) {
            p[2] = 41;
            p[3] = 0;
        }
    }
}
