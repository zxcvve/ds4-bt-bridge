/* gcc -I bridge bridge/sbc.c test/test_sbc.c -lm -o tsbc && ./tsbc */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sbc.h"

/* Frame 100 of 1 kHz (amplitude 8000) left / 3 kHz (12000) right. That stream, decoded by ffmpeg
 * (ffmpeg -f sbc -i x.sbc -f s16le out.raw), gives back both tones at full amplitude with no crosstalk, 63 dB SNR. */
static const unsigned char golden[16] = {
    0x9c, 0x75, 0x19, 0x4d, 0xc2, 0x00, 0x00, 0x00, 0x2d, 0x20, 0x00, 0x00, 0xc5, 0x59, 0x8f, 0x58
};

int main(void)
{
    struct sbc_enc e = { 0 };
    short pcm[2 * SBC_FRAME_SAMPLES];
    unsigned char f[SBC_FRAME_SIZE];
    long n = 0;

    for (int fr = 0; fr <= 100; fr++) {
        for (int i = 0; i < SBC_FRAME_SAMPLES; i++, n++) {
            pcm[2 * i] = (short)(8000 * sin(2 * M_PI * 1000 * n / 32000.0));
            pcm[2 * i + 1] = (short)(12000 * sin(2 * M_PI * 3000 * n / 32000.0));
        }
        sbc_encode(&e, pcm, f);
        assert(f[0] == 0x9C && f[1] == 0x75 && f[2] == 0x19);
    }
    assert(!memcmp(f, golden, sizeof golden));

    /* Silence: all scale factors 0 */
    struct sbc_enc z = { 0 };
    memset(pcm, 0, sizeof pcm);
    sbc_encode(&z, pcm, f);
    for (int i = 4; i < 12; i++)
        assert(f[i] == 0);

    printf("sbc: ok\n");
    return 0;
}
