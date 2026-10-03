/* gcc -I src src/ds4_translate.c test/test_translate.c -o t && ./t */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ds4_translate.h"

int main(void)
{
    unsigned char out[128];

    /* CRC32 check value */
    assert(ds4_crc32(0, (const unsigned char *)"123456789", 9) == 0xCBF43926u);

    /* Output: rumble R=0x40 L=0x80, LED blue, flash 10/20 */
    unsigned char usb_out[1 + 32] = { 0xA2, 0x05, 0xF7, 0x04, 0x00, 0x40, 0x80, 0x00, 0x00, 0xFF, 10, 20 };
    size_t n = ds4_usb_out_to_bt(usb_out, sizeof usb_out, out, sizeof out);
    assert(n == 79);
    assert(out[0] == 0xA2 && out[1] == 0x11 && out[2] == 0xC0 && out[3] == 0x00);
    assert(out[4] == 0xF7 && out[5] == 0x04);
    assert(out[7] == 0x40 && out[8] == 0x80);                   /* BT[6], BT[7]: motors */
    assert(out[9] == 0 && out[10] == 0 && out[11] == 0xFF);     /* BT[8..10]: RGB */
    assert(out[12] == 10 && out[13] == 20);                     /* BT[11..12]: flash */
    unsigned int crc = 0x7B21B325u;                             /* python zlib.crc32(0xA2 + report[0..73]) */
    assert(out[75] == (crc & 0xFF) && out[78] == (crc >> 24));

    /* Non-DS4 output passes through */
    unsigned char other[] = { 0xA2, 0x11, 0 };
    assert(ds4_usb_out_to_bt(other, sizeof other, out, sizeof out) == 0);

    /* Extended input: BT[3..65] -> USB[1..63] */
    unsigned char bt_in[1 + 78];
    for (size_t i = 0; i < sizeof bt_in; i++)
        bt_in[i] = (unsigned char)i;
    bt_in[0] = 0xA1;
    bt_in[1] = 0x11;
    n = ds4_bt_in_to_usb(bt_in, sizeof bt_in, out, sizeof out);
    assert(n == 65 && out[0] == 0xA1 && out[1] == 0x01);
    assert(out[2] == bt_in[4] && out[64] == bt_in[66]);

    /* Short input is zero-padded to USB size */
    unsigned char short_in[] = { 0xA1, 0x01, 0x80, 0x7F, 0x80, 0x7F, 0x08, 0, 0, 0, 0 };
    n = ds4_bt_in_to_usb(short_in, sizeof short_in, out, sizeof out);
    assert(n == 65 && !memcmp(out, short_in, sizeof short_in) && out[64] == 0);

    /* Calibration feature 0x05 -> 0x02 */
    unsigned char cal[1 + 41] = { 0xA3, 0x05, 0x11, 0x22 };
    cal[37] = 0x99;
    n = ds4_bt_in_to_usb(cal, sizeof cal, out, sizeof out);
    assert(n == 38 && out[1] == 0x02 && out[2] == 0x11 && out[3] == 0x22 && out[37] == 0x99);

    /* GET_REPORT feature 0x02 -> 0x05, max size bumped */
    unsigned char get[] = { 0x4B, 0x02, 37, 0 };
    ds4_fix_get_feature(get, sizeof get);
    assert(get[1] == 0x05 && get[2] == 41);
    unsigned char get_other[] = { 0x43, 0x12 };
    ds4_fix_get_feature(get_other, sizeof get_other);
    assert(get_other[1] == 0x12);

    puts("ok");
    return 0;
}
