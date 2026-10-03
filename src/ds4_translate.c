#include <string.h>
#include "ds4_translate.h"

/* Standard CRC32 (zlib). DS4 BT reports carry crc32(HIDP header byte + report[0..n-5]). */
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
    if (n < 1)
        return 0;

    if (in[0] == 0x11) {
        /* Extended report: BT[3..65] == USB[1..63]. */
        if (n < 3 + DS4_USB_INPUT_SIZE - 1 || cap < DS4_USB_INPUT_SIZE)
            return 0;
        out[0] = 0x01;
        memcpy(out + 1, in + 3, DS4_USB_INPUT_SIZE - 1);
        return DS4_USB_INPUT_SIZE;
    }

    if (in[0] == 0x01) {
        /* Short report (before extended mode kicks in): sticks/buttons/triggers share the USB layout. */
        if (cap < DS4_USB_INPUT_SIZE)
            return 0;
        size_t copy = n < DS4_USB_INPUT_SIZE ? n : DS4_USB_INPUT_SIZE;
        memset(out, 0, DS4_USB_INPUT_SIZE);
        memcpy(out, in, copy);
        return DS4_USB_INPUT_SIZE;
    }

    if (in[0] == 0x05) {
        /* Calibration: BT 0x05 is USB 0x02 plus CRC; data bytes are identical. */
        if (n < DS4_USB_CALIB_SIZE || cap < DS4_USB_CALIB_SIZE)
            return 0;
        out[0] = 0x02;
        memcpy(out + 1, in + 1, DS4_USB_CALIB_SIZE - 1);
        return DS4_USB_CALIB_SIZE;
    }

    return 0;
}

size_t ds4_usb_out_to_bt(const unsigned char *in, size_t n, unsigned char *out, size_t cap)
{
    if (n < 1 || in[0] != 0x05 || cap < DS4_BT_REPORT_SIZE)
        return 0;

    memset(out, 0, DS4_BT_REPORT_SIZE);
    out[0] = 0x11;
    out[1] = 0xC0;                       /* HID + CRC present, poll interval 0 */
    /* USB[1..31] (flags, 0x04, pad, rumble R/L, RGB, flash on/off, ...) == BT[3..33] */
    size_t copy = n - 1 < DS4_USB_OUTPUT_SIZE - 1 ? n - 1 : DS4_USB_OUTPUT_SIZE - 1;
    memcpy(out + 3, in + 1, copy);

    /* CRC covers the HIDP DATA|Output header (0xA2) that HidBth prepends on the wire. */
    unsigned char seed = 0xA2;
    unsigned int crc = ds4_crc32(0, &seed, 1);
    crc = ds4_crc32(crc, out, DS4_BT_REPORT_SIZE - 4);
    out[74] = (unsigned char)crc;
    out[75] = (unsigned char)(crc >> 8);
    out[76] = (unsigned char)(crc >> 16);
    out[77] = (unsigned char)(crc >> 24);
    return DS4_BT_REPORT_SIZE;
}
