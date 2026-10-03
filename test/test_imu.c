/* gcc -I bridge bridge/imu.c test/test_imu.c -lm -o ti && ./ti */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "imu.h"

/* ViGEmBus's feature 0x02 answer (USB layout), also in bridge/ds4bridge.c */
static const unsigned char usb_cal[37] = {
    0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x87, 0x22, 0x7B, 0xDD, 0xB2, 0x22, 0x47, 0xDD, 0xBD,
    0x22, 0x43, 0xDD, 0x1C, 0x02, 0x1C, 0x02, 0x7F, 0x1E, 0x2E, 0xDF, 0x60, 0x1F, 0x4C, 0xE0, 0x3A,
    0x1D, 0xC6, 0xDE, 0x08, 0x00
};

static double decode(const unsigned char *usb_in, const struct ds4_imu_cal *cal, int i)
{
    short raw = (short)(usb_in[13 + 2 * i] | usb_in[14 + 2 * i] << 8);
    return (raw - cal->bias[i]) * cal->scale[i];
}

int main(void)
{
    struct ds4_imu_cal u, b, bad;

    /* SDL's formulas, by hand: pitch bias 1, scale (540+540)*16 / (|8839-1| + |-8837-1|);
     * accel X range 7807-(-8402) = 16209, bias 7807 - 16209/2 = -297, scale 16384/16209 */
    assert(ds4_imu_cal_parse(usb_cal, sizeof usb_cal, 0, &u));
    assert(u.bias[0] == 1 && fabs(u.scale[0] - 17280.0 / 17676) < 1e-9);
    assert(u.bias[3] == -297 && fabs(u.scale[3] - 16384.0 / 16209) < 1e-9);

    /* The same calibration in BT order (pitch+, yaw+, roll+, pitch-, yaw-, roll-) parses identically */
    unsigned char bt_cal[41];
    memcpy(bt_cal, usb_cal, sizeof usb_cal);
    bt_cal[0] = 0x05;
    static const int from[6] = { 7, 11, 15, 9, 13, 17 };   /* USB offsets of p+, y+, r+, p-, y-, r- */
    for (int i = 0; i < 6; i++)
        memcpy(bt_cal + 7 + 2 * i, usb_cal + from[i], 2);
    assert(ds4_imu_cal_parse(bt_cal, sizeof bt_cal, 1, &b));
    for (int i = 0; i < 6; i++)
        assert(b.bias[i] == u.bias[i] && b.scale[i] == u.scale[i]);

    /* Implausible (bias 2000) or short data -> identity, like SDL */
    unsigned char broken[37];
    memcpy(broken, usb_cal, sizeof broken);
    broken[1] = 0xD0; broken[2] = 0x07;
    assert(!ds4_imu_cal_parse(broken, sizeof broken, 0, &bad));
    assert(bad.bias[0] == 0 && bad.scale[0] == 1.0 && bad.scale[5] == 1.0);
    assert(!ds4_imu_cal_parse(usb_cal, 20, 0, &bad));

    /* Remap: decoding the result with `to` gives what `from` gave for the original (within 1 LSB) */
    unsigned char in[64] = { 0x01 }, out[64];
    short raws[6] = { 1000, -2500, 30, 8100, -150, -8300 };
    for (int i = 0; i < 6; i++) {
        in[13 + 2 * i] = (unsigned char)raws[i];
        in[14 + 2 * i] = (unsigned char)((unsigned short)raws[i] >> 8);
    }
    memcpy(out, in, sizeof in);
    ds4_imu_remap(out, &bad, &u);                           /* identity pad -> ViGEm's calibration */
    for (int i = 0; i < 6; i++)
        assert(fabs(decode(out, &u, i) - decode(in, &bad, i)) <= u.scale[i] / 2 + 1e-9);
    assert(memcmp(out, in, 13) == 0 && memcmp(out + 25, in + 25, sizeof in - 25) == 0);   /* nothing else touched */

    memcpy(out, in, sizeof in);
    ds4_imu_remap(out, &u, &u);                             /* same calibration: unchanged */
    assert(memcmp(out, in, sizeof in) == 0);

    struct ds4_imu_cal big = bad;                           /* clamps instead of wrapping */
    big.scale[0] = 4.0;
    in[13] = 0xFF; in[14] = 0x7F;                           /* 32767 */
    memcpy(out, in, sizeof in);
    ds4_imu_remap(out, &big, &bad);
    assert(out[13] == 0xFF && out[14] == 0x7F);

    puts("ok");
    return 0;
}
