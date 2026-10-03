/* WASAPI loopback capture of one output device, converted by Windows to the pad's 32 kHz stereo s16. */
#pragma once

/* Records what plays on the active output device whose name contains `name` (UTF-8, any case) and calls sink with
 * every `chunk` frames (interleaved stereo s16 at 32 kHz). Runs on the calling thread; returns only on an error,
 * which it prints. */
void audio_capture(const char *name, int chunk, void (*sink)(const short *pcm));
