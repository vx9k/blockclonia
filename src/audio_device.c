/* The audio output device through miniaudio's low-level device API.
 *
 * Only playback, in float, in the mixer's own format: miniaudio converts
 * to whatever the platform's device wants (and picks the backend at run
 * time: PipeWire/PulseAudio/ALSA, WASAPI, CoreAudio, AAudio/OpenSL). The
 * period is ~10 ms, a latency the ear does not notice for effects while
 * leaving the mixer long buffers. The game's mixer already limits its
 * output to [-1, 1], so miniaudio's own clipping is switched off. */
#include "audio_device.h"
#include "log.h"
#include "mem.h"

#include "miniaudio.h"

#include <stdio.h>
#include <string.h>

struct audio_device {
    ma_device dev;
    audio_render_fn render;
    void *user;
    int channels;
    char name[300]; /* backend name + a device name of up to 255 chars */
};

static void data_cb(ma_device *dev, void *out, const void *in, ma_uint32 frames)
{
    (void)in;
    audio_device *d = dev->pUserData;
    d->render(d->user, out, frames);
}

audio_device *audio_device_open(audio_render_fn render, void *user, uint32_t rate, int channels)
{
    audio_device *d = mem_calloc(1, sizeof *d);
    d->render = render;
    d->user = user;
    d->channels = channels;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = (ma_uint32)channels;
    cfg.sampleRate = rate;
    cfg.dataCallback = data_cb;
    cfg.pUserData = d;
    cfg.periodSizeInMilliseconds = 10;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.noClip = MA_TRUE;                    /* the mixer limits itself */
    cfg.noPreSilencedOutputBuffer = MA_TRUE; /* the mixer writes every sample */
    ma_result res = ma_device_init(NULL, &cfg, &d->dev);
    if (res != MA_SUCCESS) {
        log_warn("sound: no output device (%s)", ma_result_description(res));
        mem_free(d);
        return NULL;
    }
    snprintf(d->name, sizeof d->name, "%s: %s", ma_get_backend_name(d->dev.pContext->backend), d->dev.playback.name);
    res = ma_device_start(&d->dev);
    if (res != MA_SUCCESS) {
        log_warn("sound: could not start %s (%s)", d->name, ma_result_description(res));
        ma_device_uninit(&d->dev);
        mem_free(d);
        return NULL;
    }
    return d;
}

void audio_device_close(audio_device *d)
{
    if (!d) return;
    ma_device_uninit(&d->dev); /* stops the callback before returning */
    mem_free(d);
}

const char *audio_device_name(const audio_device *d) { return d ? d->name : "none"; }
