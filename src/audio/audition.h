/* The listening tool behind --sound-render: every sound, take by take,
 * played through the real mixer in a few fixed settings and written as
 * 16-bit stereo WAV files, with a summary of each (length, peak, loudness,
 * brightness) in index.csv. A change to a sound or to the mixer can then be
 * heard and measured before and after (docs/design/sound.md, S0).
 *
 * Headless: it mixes into memory and writes files, and never opens an
 * audio device. The settings are the listener's place relative to the
 * source; rooms and caves join them once the mixer knows about the world
 * around the listener. */
#ifndef MC_AUDITION_H
#define MC_AUDITION_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    float seconds;
    float peak_db;     /* dBFS of the loudest sample; AUDITION_SILENT_DB for silence */
    float rms_db;      /* dBFS of the whole clip's RMS */
    float centroid_hz; /* spectral centroid: where the energy sits, a measure of brightness */
} audition_stats;

#define AUDITION_SILENT_DB (-120.0f)

/* Interleaved stereo float frames as a 16-bit PCM WAV file in *out
 * (mem_alloc'd, the caller frees it). Returns the file's size. */
size_t audition_wav(const float *stereo, uint32_t frames, uint32_t rate, uint8_t **out);

/* Measures interleaved stereo float frames (their mono mix). */
void audition_measure(const float *stereo, uint32_t frames, uint32_t rate, audition_stats *st);

/* Renders every take into dir (made if missing): dir/<setting>/<sound>.wav
 * and dir/index.csv. Deterministic for a seed. Returns the number of WAV
 * files written, or -1 if dir or a file could not be written. */
int audition_render(const char *dir, uint32_t seed);

#endif
