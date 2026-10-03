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

/* Input CRCs are not verified here; ds4_bt_in_valid does that for callers that want SDL's filtering. */
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

    return 0;
}

size_t ds4_usb_out_to_bt(const unsigned char *in, size_t n, unsigned char *out, size_t cap, unsigned char interval_ms)
{
    if (n < 1 || in[0] != 0x05 || cap < DS4_BT_REPORT_SIZE)
        return 0;

    memset(out, 0, DS4_BT_REPORT_SIZE);
    out[0] = 0x11;
    out[1] = 0xC0 | (interval_ms & 0x0F); /* HID + CRC present, input report interval */
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

void ds4_bt_audio_report(unsigned short frame, unsigned char target, const unsigned char sbc[DS4_BT_AUDIO_SBC],
                         unsigned char out[DS4_BT_AUDIO_SIZE])
{
    memset(out, 0, DS4_BT_AUDIO_SIZE);
    out[0] = 0x17;
    out[1] = 0x40;      /* CRC present, no HID (controller state) data */
    out[2] = 0xA0;
    out[3] = (unsigned char)frame;
    out[4] = (unsigned char)(frame >> 8);
    out[5] = target;
    memcpy(out + 6, sbc, DS4_BT_AUDIO_SBC);
    unsigned char seed = 0xA2;
    unsigned int crc = ds4_crc32(ds4_crc32(0, &seed, 1), out, DS4_BT_AUDIO_SIZE - 4);
    for (int i = 0; i < 4; i++)
        out[DS4_BT_AUDIO_SIZE - 4 + i] = (unsigned char)(crc >> 8 * i);
}

int ds4_parse_mac(const wchar_t *s, unsigned long long *addr)
{
    unsigned long long a = 0;
    int digits = 0;
    for (; *s; s++) {
        int d = *s >= L'0' && *s <= L'9' ? *s - L'0' : *s >= L'a' && *s <= L'f' ? *s - L'a' + 10 :
                *s >= L'A' && *s <= L'F' ? *s - L'A' + 10 : -1;
        if (d < 0 && (*s == L':' || *s == L'-'))
            continue;
        if (d < 0 || ++digits > 12)
            return 0;
        a = a << 4 | (unsigned)d;
    }
    if (digits != 12)
        return 0;
    *addr = a;
    return 1;
}

int ds4_battery_percent(const unsigned char *usb_in, int *cable)
{
    unsigned char level = usb_in[30] & 0x0F;
    *cable = (usb_in[30] & 0x10) != 0;
    if (*cable && level > 11)
        return -1;
    return level >= 10 ? 100 : level * 10 + 5;     /* 11 = full on cable */
}

void ds4_usb_out_dim_led(unsigned char *usb_out, unsigned char led[3], int percent)
{
    if (usb_out[1] & 0x02)
        memcpy(led, usb_out + 6, 3);
    usb_out[1] |= 0x02;
    for (int i = 0; i < 3; i++)
        usb_out[6 + i] = (unsigned char)(led[i] * percent / 100);
}

int ds4_bt_in_valid(const unsigned char *in, size_t n, unsigned *good_crcs)
{
    if (n >= 1 && in[0] == 0x01)
        return 1;
    if (n < DS4_BT_REPORT_SIZE || in[0] != 0x11 || !(in[1] & 0x80))
        return 0;

    /* CRC covers the HIDP DATA|Input header (0xA1) plus the report. */
    unsigned char seed = 0xA1;
    unsigned int crc = ds4_crc32(ds4_crc32(0, &seed, 1), in, DS4_BT_REPORT_SIZE - 4);
    unsigned int got = in[74] | in[75] << 8 | in[76] << 16 | (unsigned int)in[77] << 24;
    if (crc == got) {
        ++*good_crcs;
        return 1;
    }
    if (*good_crcs > 0)
        --*good_crcs;
    return *good_crcs < 3;  /* a pad that never sends valid CRCs still works */
}
