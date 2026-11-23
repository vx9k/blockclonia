/* The audio output device: opens the platform's default playback device
 * and calls a render function from its thread for interleaved float
 * frames. audio_device.c uses miniaudio; audio_null.c (builds without
 * sound) opens nothing. */
#ifndef MC_AUDIO_DEVICE_H
#define MC_AUDIO_DEVICE_H

#include <stdint.h>

typedef struct audio_device audio_device;

typedef void (*audio_render_fn)(void *user, float *out, uint32_t frames);

/* rate and channels are what render produces. Returns NULL when there is
 * no usable device (the game plays on in silence). */
audio_device *audio_device_open(audio_render_fn render, void *user, uint32_t rate, int channels);
void audio_device_close(audio_device *d);
/* "PipeWire (Pulse) / Built-in Audio", or "none". */
const char *audio_device_name(const audio_device *d);

#endif
