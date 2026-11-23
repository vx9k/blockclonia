/* Builds without sound (MC_SOUND=OFF, or no miniaudio): no device. */
#include "audio_device.h"

#include <stddef.h>

audio_device *audio_device_open(audio_render_fn render, void *user, uint32_t rate, int channels)
{
    (void)render;
    (void)user;
    (void)rate;
    (void)channels;
    return NULL;
}

void audio_device_close(audio_device *d) { (void)d; }

const char *audio_device_name(const audio_device *d)
{
    (void)d;
    return "none (built without sound)";
}
