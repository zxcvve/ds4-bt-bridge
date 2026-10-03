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
    unsigned char usb_out[32] = { 0x05, 0xF7, 0x04, 0x00, 0x40, 0x80, 0x00, 0x00, 0xFF, 10, 20 };
    size_t n = ds4_usb_out_to_bt(usb_out, sizeof usb_out, out, sizeof out, 0);
    assert(n == 78);
    assert(out[0] == 0x11 && out[1] == 0xC0 && out[2] == 0x00);
    assert(out[3] == 0xF7 && out[4] == 0x04);
    assert(out[6] == 0x40 && out[7] == 0x80);                   /* motors */
    assert(out[8] == 0 && out[9] == 0 && out[10] == 0xFF);      /* RGB */
    assert(out[11] == 10 && out[12] == 20);                     /* flash */
    unsigned int crc = 0x7B21B325u;                             /* python zlib.crc32(0xA2 + report[0..73]) */
    assert(out[74] == (crc & 0xFF) && out[77] == (crc >> 24));

    /* Non-DS4 output passes through */
    unsigned char other[] = { 0x11, 0 };
    assert(ds4_usb_out_to_bt(other, sizeof other, out, sizeof out, 0) == 0);

    /* Report interval goes into the low bits of byte 1, inside the CRC */
    n = ds4_usb_out_to_bt(usb_out, sizeof usb_out, out, sizeof out, 4);
    assert(n == 78 && out[1] == 0xC4);
    unsigned char hdr = 0xA2;
    unsigned int icrc = ds4_crc32(ds4_crc32(0, &hdr, 1), out, 74);
    assert(out[74] == (icrc & 0xFF) && out[77] == (icrc >> 24) && icrc != crc);

    /* Extended input: BT[3..65] -> USB[1..63] */
    unsigned char bt_in[78];
    for (size_t i = 0; i < sizeof bt_in; i++)
        bt_in[i] = (unsigned char)i;
    bt_in[0] = 0x11;
    n = ds4_bt_in_to_usb(bt_in, sizeof bt_in, out, sizeof out);
    assert(n == 64 && out[0] == 0x01);
    assert(out[1] == bt_in[3] && out[63] == bt_in[65]);

    /* Short input is zero-padded to USB size */
    unsigned char short_in[] = { 0x01, 0x80, 0x7F, 0x80, 0x7F, 0x08, 0, 0, 0, 0 };
    n = ds4_bt_in_to_usb(short_in, sizeof short_in, out, sizeof out);
    assert(n == 64 && !memcmp(out, short_in, sizeof short_in) && out[63] == 0);

    /* Calibration feature 0x05 -> 0x02 */
    unsigned char cal[41] = { 0x05, 0x11, 0x22 };
    cal[36] = 0x99;
    n = ds4_bt_in_to_usb(cal, sizeof cal, out, sizeof out);
    assert(n == 37 && out[0] == 0x02 && out[1] == 0x11 && out[2] == 0x22 && out[36] == 0x99);

    /* Battery byte 30: low nibble level, bit 4 cable (SDL mapping) */
    unsigned char usb_in[64] = { 0x01 };
    int cable;
    usb_in[30] = 0x03;
    assert(ds4_battery_percent(usb_in, &cable) == 35 && !cable);
    usb_in[30] = 0x0A;
    assert(ds4_battery_percent(usb_in, &cable) == 100 && !cable);
    usb_in[30] = 0x15;
    assert(ds4_battery_percent(usb_in, &cable) == 55 && cable);
    usb_in[30] = 0x1B;
    assert(ds4_battery_percent(usb_in, &cable) == 100 && cable);
    usb_in[30] = 0x1E;
    assert(ds4_battery_percent(usb_in, &cable) == -1);

    /* Light bar: game color is remembered and scaled; reports without the LED flag reuse it */
    unsigned char led[3] = { 0, 0, 0x40 };
    unsigned char game[32] = { 0x05, 0x03, 0x04, 0, 0x10, 0x20, 200, 100, 50 };
    ds4_usb_out_dim_led(game, led, 50);
    assert(led[0] == 200 && led[1] == 100 && led[2] == 50);
    assert(game[6] == 100 && game[7] == 50 && game[8] == 25 && game[4] == 0x10 && game[5] == 0x20);
    unsigned char rumble_only[32] = { 0x05, 0x01, 0x04, 0, 0x30, 0x40 };
    ds4_usb_out_dim_led(rumble_only, led, 0);
    assert(rumble_only[1] == 0x03 && rumble_only[6] == 0 && rumble_only[8] == 0 && led[0] == 200);
    assert(rumble_only[4] == 0x30 && rumble_only[5] == 0x40);

    /* Input validation: HID flag + CRC (seed 0xA1), SDL's tolerance for pads without valid CRCs */
    unsigned char v[78];
    for (size_t i = 0; i < sizeof v; i++)
        v[i] = (unsigned char)i;
    v[0] = 0x11;
    v[1] = 0xC0;
    unsigned int vcrc = 0x2B9723F3u;                            /* python zlib.crc32(b"\xa1" + v[:74]) */
    v[74] = (unsigned char)vcrc; v[75] = (unsigned char)(vcrc >> 8);
    v[76] = (unsigned char)(vcrc >> 16); v[77] = (unsigned char)(vcrc >> 24);
    unsigned good = 0;
    assert(ds4_bt_in_valid(v, sizeof v, &good) && good == 1);
    v[10] ^= 1;                                                 /* corrupt: still passes until CRCs proved reliable */
    assert(ds4_bt_in_valid(v, sizeof v, &good) && good == 0);
    v[10] ^= 1;
    for (int i = 0; i < 4; i++)
        assert(ds4_bt_in_valid(v, sizeof v, &good));
    v[10] ^= 1;
    assert(!ds4_bt_in_valid(v, sizeof v, &good) && good == 3); /* 4 good, then a bad one is dropped */
    v[10] ^= 1;
    v[1] = 0x40;                                                /* no HID data flag */
    assert(!ds4_bt_in_valid(v, sizeof v, &good));
    assert(!ds4_bt_in_valid(v, 70, &good));                     /* too short */
    assert(ds4_bt_in_valid(short_in, sizeof short_in, &good));  /* basic 0x01 report */

    puts("ok");
    return 0;
}
