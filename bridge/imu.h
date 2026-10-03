/* DS4 gyro/accel calibration, SDL's math. */
#pragma once
#include <stddef.h>

/* IMU calibration in SDL's units: physical = (raw - bias) * scale; gyro, then accel, X/Y/Z. */
struct ds4_imu_cal {
    int bias[6];
    double scale[6];
};

/* Parses calibration (USB feature 0x02 layout, or BT 0x05 when bt != 0; gyro plus/minus are ordered differently).
 * Returns 0 and fills cal with identity (what SDL falls back to) if the data is implausible. */
int ds4_imu_cal_parse(const unsigned char *rep, size_t n, int bt, struct ds4_imu_cal *cal);

/* Rewrites gyro/accel of USB input 0x01 (bytes 13..24) so that decoding them with `to` gives what `from` gives
 * for the original values. */
void ds4_imu_remap(unsigned char *usb_in, const struct ds4_imu_cal *from, const struct ds4_imu_cal *to);
