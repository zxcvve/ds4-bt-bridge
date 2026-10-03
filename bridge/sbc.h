/*
 * Minimal SBC encoder, fixed to what the DS4 speaker takes over Bluetooth (frame header 9C 75 19):
 * 32 kHz, 16 blocks, dual channel, loudness allocation, 8 subbands, bitpool 25. Float math, A2DP spec appendix B.
 * Pure C, no Windows dependencies, so the test builds anywhere.
 */
#pragma once

#define SBC_FRAME_SAMPLES 128   /* per channel: 16 blocks x 8 subbands = 4 ms at 32 kHz */
#define SBC_FRAME_SIZE    112

struct sbc_enc { float x[2][80]; };    /* analysis filter history per channel; zero-initialize */

/* pcm: SBC_FRAME_SAMPLES interleaved stereo samples (L, R, L, R, ...). Writes SBC_FRAME_SIZE bytes to out. */
void sbc_encode(struct sbc_enc *e, const short *pcm, unsigned char *out);
