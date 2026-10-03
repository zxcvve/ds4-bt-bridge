#include <math.h>
#include <string.h>
#include "sbc.h"

#define BLOCKS   16
#define SUBBANDS 8
#define BITPOOL  25

/* Analysis window for 8 subbands, A2DP spec table 12.24. */
static const float proto[80] = {
    0.00000000e+00f, 1.56575398e-04f, 3.43256425e-04f, 5.54620202e-04f,
    8.23919506e-04f, 1.13992507e-03f, 1.47640169e-03f, 1.78371725e-03f,
    2.01182542e-03f, 2.10371989e-03f, 1.99454554e-03f, 1.61656283e-03f,
    9.02154502e-04f, -1.78805361e-04f, -1.64973098e-03f, -3.49717454e-03f,
    5.65949473e-03f, 8.02941163e-03f, 1.04584443e-02f, 1.27472335e-02f,
    1.46525263e-02f, 1.59045603e-02f, 1.62208471e-02f, 1.53184106e-02f,
    1.29371806e-02f, 8.85757540e-03f, 2.92408442e-03f, -4.91578024e-03f,
    -1.46404076e-02f, -2.61098752e-02f, -3.90751381e-02f, -5.31873032e-02f,
    6.79989431e-02f, 8.29847578e-02f, 9.75753918e-02f, 1.11196689e-01f,
    1.23264548e-01f, 1.33264415e-01f, 1.40753505e-01f, 1.45389847e-01f,
    1.46955068e-01f, 1.45389847e-01f, 1.40753505e-01f, 1.33264415e-01f,
    1.23264548e-01f, 1.11196689e-01f, 9.75753918e-02f, 8.29847578e-02f,
    -6.79989431e-02f, -5.31873032e-02f, -3.90751381e-02f, -2.61098752e-02f,
    -1.46404076e-02f, -4.91578024e-03f, 2.92408442e-03f, 8.85757540e-03f,
    1.29371806e-02f, 1.53184106e-02f, 1.62208471e-02f, 1.59045603e-02f,
    1.46525263e-02f, 1.27472335e-02f, 1.04584443e-02f, 8.02941163e-03f,
    -5.65949473e-03f, -3.49717454e-03f, -1.64973098e-03f, -1.78805361e-04f,
    9.02154502e-04f, 1.61656283e-03f, 1.99454554e-03f, 2.10371989e-03f,
    2.01182542e-03f, 1.78371725e-03f, 1.47640169e-03f, 1.13992507e-03f,
    8.23919506e-04f, 5.54620202e-04f, 3.43256425e-04f, 1.56575398e-04f,
};

/* Loudness offsets for 8 subbands at 32 kHz (spec appendix B). */
static const int offset[SUBBANDS] = { -3, 0, 0, 0, 0, 0, 1, 2 };

/* One block: 8 new samples of one channel (stride 2) -> 8 subband samples. */
static void analyze(float *x, const short *pcm, float *s)
{
    static float m[SUBBANDS][16];
    if (m[0][0] == 0)
        for (int k = 0; k < SUBBANDS; k++)
            for (int i = 0; i < 16; i++)
                m[k][i] = (float)cos((k + 0.5) * (i - 4) * 3.14159265358979 / 8);

    memmove(x + 8, x, 72 * sizeof *x);
    for (int i = 0; i < 8; i++)
        x[7 - i] = pcm[2 * i];
    float y[16] = { 0 };
    for (int i = 0; i < 80; i++)
        y[i % 16] += proto[i] * x[i];
    for (int k = 0; k < SUBBANDS; k++) {
        s[k] = 0;
        for (int i = 0; i < 16; i++)
            s[k] += m[k][i] * y[i];
    }
}

/* Loudness bit allocation for one channel (dual channel allocates each channel on its own). */
static void allocate(const int *sf, int *bits)
{
    int need[SUBBANDS], max = -100;
    for (int s = 0; s < SUBBANDS; s++) {
        int loud = sf[s] - offset[s];
        need[s] = sf[s] == 0 ? -5 : loud > 0 ? loud / 2 : loud;
        if (need[s] > max)
            max = need[s];
    }

    int count = 0, slices = 0, slice = max + 1;
    do {
        slice--;
        count += slices;
        slices = 0;
        for (int s = 0; s < SUBBANDS; s++) {
            if (need[s] > slice + 1 && need[s] < slice + 16)
                slices++;
            else if (need[s] == slice + 1)
                slices += 2;
        }
    } while (count + slices < BITPOOL);
    if (count + slices == BITPOOL) {
        count += slices;
        slice--;
    }

    for (int s = 0; s < SUBBANDS; s++)
        bits[s] = need[s] < slice + 2 ? 0 : need[s] - slice < 16 ? need[s] - slice : 16;
    for (int s = 0; count < BITPOOL && s < SUBBANDS; s++) {
        if (bits[s] >= 2 && bits[s] < 16) {
            bits[s]++;
            count++;
        } else if (need[s] == slice + 1 && BITPOOL > count + 1) {
            bits[s] = 2;
            count += 2;
        }
    }
    for (int s = 0; count < BITPOOL && s < SUBBANDS; s++)
        if (bits[s] < 16) {
            bits[s]++;
            count++;
        }
}

struct bitwriter { unsigned char *p; unsigned acc; int n; };

static void put(struct bitwriter *b, unsigned v, int n)
{
    while (n--) {
        b->acc = b->acc << 1 | (v >> n & 1);
        if (++b->n == 8) {
            *b->p++ = (unsigned char)b->acc;
            b->acc = b->n = 0;
        }
    }
}

/* CRC-8, polynomial x^8+x^4+x^3+x^2+1, MSB first. */
static unsigned crc8(unsigned crc, const unsigned char *p, int n)
{
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80 ? crc << 1 ^ 0x1D : crc << 1) & 0xFF;
    }
    return crc;
}

void sbc_encode(struct sbc_enc *e, const short *pcm, unsigned char *out)
{
    float sb[2][BLOCKS][SUBBANDS];
    int sf[2][SUBBANDS], bits[2][SUBBANDS];

    for (int blk = 0; blk < BLOCKS; blk++)
        for (int ch = 0; ch < 2; ch++)
            analyze(e->x[ch], pcm + blk * 2 * SUBBANDS + ch, sb[ch][blk]);
    for (int ch = 0; ch < 2; ch++) {
        for (int s = 0; s < SUBBANDS; s++) {
            float max = 0;
            for (int blk = 0; blk < BLOCKS; blk++)
                max = fmaxf(max, fabsf(sb[ch][blk][s]));
            sf[ch][s] = 0;      /* smallest with |sample| < 2^(sf+1) */
            while (sf[ch][s] < 15 && max >= (float)(2 << sf[ch][s]))
                sf[ch][s]++;
        }
        allocate(sf[ch], bits[ch]);
    }

    memset(out, 0, SBC_FRAME_SIZE);
    out[0] = 0x9C;
    out[1] = 0x75;
    out[2] = BITPOOL;
    struct bitwriter b = { out + 4, 0, 0 };
    for (int ch = 0; ch < 2; ch++)
        for (int s = 0; s < SUBBANDS; s++)
            put(&b, (unsigned)sf[ch][s], 4);
    for (int blk = 0; blk < BLOCKS; blk++)
        for (int ch = 0; ch < 2; ch++)
            for (int s = 0; s < SUBBANDS; s++) {
                if (!bits[ch][s])
                    continue;
                int levels = (1 << bits[ch][s]) - 1;
                int q = (int)((sb[ch][blk][s] / (float)(2 << sf[ch][s]) + 1) * levels / 2);
                put(&b, (unsigned)(q < 0 ? 0 : q > levels ? levels : q), bits[ch][s]);
            }
    put(&b, 0, (8 - b.n) & 7);
    out[3] = (unsigned char)crc8(crc8(0x0F, out + 1, 2), out + 4, 8);    /* header + scale factors */
}
