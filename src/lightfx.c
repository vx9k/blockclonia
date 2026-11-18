#include "lightfx.h"
#include "block.h"
#include "item.h"
#include "light.h"
#include "thermo.h"
#include <math.h>

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float smoothf(float v, float a, float b)
{
    float t = clamp01((v - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

/* Level 0..1 to brightness, the light curve (light.h). */
static float bright(float level) { return exp2f(LIGHT_CURVE_K * level - LIGHT_CURVE_K); }

/* Block light's gain as a shift of its level. */
static float gain_shift(void) { return log2f(LIGHTFX_BLOCK_GAIN) / LIGHT_CURVE_K; }

void lightfx_flash(lightfx_frame *f, float flash)
{
    /* Burning powder gas, white with a little yellow. */
    static const float RGB[3] = {1.0f, 0.82f, 0.55f};
    float k = isfinite(flash) ? fminf(fmaxf(flash, 0.0f), 1.0f) : 0.0f;
    for (int i = 0; i < 3; i++) f->held[i] = fmaxf(f->held[i], k * RGB[i]);
}

float lightfx_flicker(double t)
{
    /* Three incommensurate sines, so the pattern doesn't visibly repeat. */
    double n = 0.5 * sin(t * 11.3) + 0.3 * sin(t * 17.9 + 1.3) + 0.2 * sin(t * 5.1 + 0.4);
    return (float)(0.95 + 0.05 * n);
}

void lightfx_build(lightfx_frame *f, const float sun_dir[3], float daylight, uint32_t held_item, int brightness,
                   double time, int flicker)
{
    /* Light through a long path of air at sunrise and sunset has lost its
     * blue; moonlight looks blue to dark-adapted eyes (the Purkinje
     * shift). */
    static const float WARM[3] = {1.0f, 0.72f, 0.45f}, MOON[3] = {0.62f, 0.75f, 1.0f};
    float e = sun_dir[1];
    /* Blue only once the sun is below the horizon, so a sunrise is warm
     * while the sky is still dim. */
    float moon = smoothf(-e, 0.0f, 0.15f);
    float warm = expf(-((e - 0.05f) / 0.12f) * ((e - 0.05f) / 0.12f));
    for (int i = 0; i < 3; i++) {
        float tint = 1.0f + (WARM[i] - 1.0f) * warm;
        tint += (MOON[i] - tint) * moon;
        f->sky[i] = clamp01(daylight * tint);
    }
    /* Faces shade by the sun's direction once it is a little above the
     * horizon, by the moon's at night, and evenly in twilight. */
    float c = e >= 0.0f ? smoothf(e, 0.0f, 0.2f) : -LIGHTFX_MOON_CONTRAST * smoothf(-e, 0.0f, 0.2f);
    for (int i = 0; i < 3; i++) f->sun[i] = sun_dir[i] * c;
    /* A carried light is a level below its block: the hand holds it half a
     * metre from the eye. */
    uint8_t block = held_item ? item_get(held_item)->block : B_AIR;
    if (held_item && item_is_block(held_item)) block = (uint8_t)held_item;
    uint16_t em = light_emission(block);
    float flick = block == B_FOXFIRE ? 1.0f : (flicker ? lightfx_flicker(time) : LIGHTFX_STEADY);
    const unsigned lv[3] = {LIGHT_R(em), LIGHT_G(em), LIGHT_B(em)};
    for (int i = 0; i < 3; i++) f->held[i] = lv[i] > 1 ? (float)(lv[i] - 1) / (float)LIGHT_MAX * flick : 0.0f;
    f->floor = clamp01((float)brightness / 100.0f) * LIGHTFX_FLOOR_MAX;
}

float lightfx_face(const lightfx_frame *f, int face)
{
    float n[3] = {0.0f, 0.0f, 0.0f};
    n[face >> 1] = (face & 1) ? -1.0f : 1.0f;
    float c = sqrtf(f->sun[0] * f->sun[0] + f->sun[1] * f->sun[1] + f->sun[2] * f->sun[2]);
    float d = n[0] * f->sun[0] + n[1] * f->sun[1] + n[2] * f->sun[2];
    return 1.0f - LIGHTFX_SUN_SHADE * (c - (d > 0.0f ? d : 0.0f));
}

uint32_t lightfx_face_code(const lightfx_frame *f, int face)
{
    float shift = -log2f(lightfx_face(f, face)) / LIGHT_CURVE_K;
    return (uint32_t)lroundf(shift / LIGHTFX_SUN_STEP);
}

float lightfx_tone(float x)
{
    float u = x > LIGHTFX_TONE_KNEE ? x - LIGHTFX_TONE_KNEE : 0.0f;
    float k = x < LIGHTFX_TONE_KNEE ? x : LIGHTFX_TONE_KNEE;
    return k + u / (1.0f + u / (1.0f - LIGHTFX_TONE_KNEE));
}

float lightfx_at(const lightfx_frame *f, uint16_t l, float dist)
{
    unsigned rgb = LIGHT_R(l);
    if (LIGHT_G(l) > rgb) rgb = LIGHT_G(l);
    if (LIGHT_B(l) > rgb) rgb = LIGHT_B(l);
    float held = fmaxf(f->held[0], fmaxf(f->held[1], f->held[2])) - dist / (float)LIGHT_MAX;
    float blk = fmaxf((float)rgb / (float)LIGHT_MAX, held) + gain_shift();
    float sky = fmaxf(f->sky[0], fmaxf(f->sky[1], f->sky[2]));
    float sky_lv = (float)LIGHT_SKY(l) / (float)LIGHT_MAX + log2f(fmaxf(sky, 1e-3f)) / LIGHT_CURVE_K;
    return bright(fmaxf(fmaxf(blk, sky_lv), f->floor));
}

uint32_t lightfx_pack(const lightfx_frame *f, float daylight, float sky[3])
{
    int has_sky = f->sky[0] > 0.0f || f->sky[1] > 0.0f || f->sky[2] > 0.0f;
    for (int i = 0; i < 3; i++) {
        float s = has_sky ? f->sky[i] : daylight;
        sky[i] = log2f(s > 1e-3f ? s : 1e-3f) / LIGHT_CURVE_K;
    }
    const float lv[4] = {f->held[0], f->held[1], f->held[2], f->floor};
    uint32_t bits = 0;
    for (int i = 0; i < 4; i++) bits |= (uint32_t)lroundf(clamp01(lv[i]) * 255.0f) << (8 * i);
    return bits;
}

void lightfx_fog(const lightfx_frame *f, const float sky[3], uint16_t eye, float out[3])
{
    /* Fog is light scattered along the view: the open sky's where the eye
     * sees it, a share of the fire's where that is brighter. Never black,
     * so far caves still fade out rather than cut off. */
    float open = fmaxf(bright((float)LIGHT_SKY(eye) / (float)LIGHT_MAX), bright(f->floor));
    if (open < 0.06f) open = 0.06f;
    const unsigned lv[3] = {LIGHT_R(eye), LIGHT_G(eye), LIGHT_B(eye)};
    for (int i = 0; i < 3; i++) {
        float blk = lv[i] ? 0.3f * bright((float)lv[i] / (float)LIGHT_MAX) : 0.0f;
        out[i] = clamp01(fmaxf(sky[i] * open, blk));
    }
}

void lightfx_ease(float cur[3], const float target[3], float dt)
{
    float k = 1.0f - expf(-(dt > 0.0f ? dt : 0.0f) / 1.5f);
    for (int i = 0; i < 3; i++) cur[i] += (target[i] - cur[i]) * k;
}
