/* Placeholder until the mixer lands: silent. */
#include "sound.h"
#include "mem.h"

#include <string.h>

struct sound {
    int unused;
};

sound *sound_create(uint32_t seed, uint32_t rate, int channels)
{
    (void)seed;
    (void)rate;
    (void)channels;
    return mem_calloc(1, sizeof(sound));
}

void sound_destroy(sound *s) { mem_free(s); }
void sound_set_volumes(sound *s, float master, float effects, float ambient)
{
    (void)s;
    (void)master;
    (void)effects;
    (void)ambient;
}
void sound_listener(sound *s, dvec3 pos, float yaw, float pitch, int underwater)
{
    (void)s;
    (void)pos;
    (void)yaw;
    (void)pitch;
    (void)underwater;
}
void sound_play(sound *s, int id, const dvec3 *pos, uint8_t material, float intensity)
{
    (void)s;
    (void)id;
    (void)pos;
    (void)material;
    (void)intensity;
}
void sound_ambience(sound *s, float wind, float water)
{
    (void)s;
    (void)wind;
    (void)water;
}
void sound_fires(sound *s, const dvec3 *pos, int n)
{
    (void)s;
    (void)pos;
    (void)n;
}
void sound_update(sound *s, float dt)
{
    (void)s;
    (void)dt;
}
void sound_render(void *s, float *out, uint32_t frames)
{
    (void)s;
    memset(out, 0, (size_t)frames * SOUND_CHANNELS * sizeof(float));
}
void sound_stats(const sound *s, int *voices, float *load)
{
    (void)s;
    *voices = 0;
    *load = 0.0f;
}
