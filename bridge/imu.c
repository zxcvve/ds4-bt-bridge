#include <stdlib.h>
#include "imu.h"

static int load16(const unsigned char *p)
{
    return (short)(p[0] | p[1] << 8);
}

int ds4_imu_cal_parse(const unsigned char *rep, size_t n, int bt, struct ds4_imu_cal *cal)
{
    /* Report offsets of pitch/yaw/roll plus and minus: BT pairs come after all three pluses. */
    static const int usb_off[6] = { 7, 11, 15, 9, 13, 17 }, bt_off[6] = { 7, 9, 11, 13, 15, 17 };
    const int *off = bt ? bt_off : usb_off;

    for (int i = 0; i < 6; i++) {
        cal->bias[i] = 0;
        cal->scale[i] = 1.0;
    }
    if (n < 35)
        return 0;

    struct ds4_imu_cal c;
    double speed = (load16(rep + 19) + load16(rep + 21)) * 16.0;   /* gyro: 1/16 deg/s per LSB */
    for (int i = 0; i < 3; i++) {
        int b = load16(rep + 1 + 2 * i);
        double d = abs(load16(rep + off[i]) - b) + abs(load16(rep + off[i + 3]) - b);
        if (d == 0)
            return 0;
        c.bias[i] = b;
        c.scale[i] = speed / d;
    }
    for (int i = 0; i < 3; i++) {
        int plus = load16(rep + 23 + 4 * i), range = plus - load16(rep + 25 + 4 * i);
        if (range == 0)
            return 0;
        c.bias[3 + i] = (short)(plus - range / 2);
        c.scale[3 + i] = 2.0 * 8192 / range;    /* accel: 1/8192 g per LSB */
    }
    /* SDL ignores calibrations like these as broken. */
    for (int i = 0; i < 6; i++)
        if (abs(c.bias[i]) > 1024 || c.scale[i] < 0.5 || c.scale[i] > 1.5)
            return 0;
    *cal = c;
    return 1;
}

void ds4_imu_remap(unsigned char *usb_in, const struct ds4_imu_cal *from, const struct ds4_imu_cal *to)
{
    for (int i = 0; i < 6; i++) {
        unsigned char *p = usb_in + 13 + 2 * i;
        double v = (load16(p) - from->bias[i]) * from->scale[i] / to->scale[i] + to->bias[i];
        long r = v < -32768 ? -32768 : v > 32767 ? 32767 : (long)(v < 0 ? v - 0.5 : v + 0.5);
        p[0] = (unsigned char)r;
        p[1] = (unsigned char)((unsigned long)r >> 8);
    }
}
