#include "sky.h"
#include "thermo.h"
#include <math.h>

#define SKY_PI 3.14159265f

/* xorshift32: a fixed, portable sequence for a given seed. */
static float rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)(x >> 8) / 16777216.0f;
}

/* Star colours by surface temperature, as the eye sees them: hot blue-white
 * stars are few but bright, most are white to yellow, a few cool orange. */
static void star_colour(float t, float rgb[3])
{
    static const float C[4][3] = {{0.72f, 0.82f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.93f, 0.78f}, {1.0f, 0.76f, 0.55f}};
    int k = t < 0.12f ? 0 : (t < 0.62f ? 1 : (t < 0.9f ? 2 : 3));
    for (int i = 0; i < 3; i++) rgb[i] = C[k][i];
}

void sky_stars(uint32_t seed, sky_star *out)
{
    uint32_t s = seed * 2654435761u | 1u;
    /* The Milky Way's plane, tilted across the sky. */
    const float pole[3] = {0.35f, 0.48f, 0.80f};
    float pl = sqrtf(pole[0] * pole[0] + pole[1] * pole[1] + pole[2] * pole[2]);
    const float n[3] = {pole[0] / pl, pole[1] / pl, pole[2] / pl};
    /* Two unit vectors spanning that plane. */
    float a[3] = {n[1], -n[0], 0.0f};
    float al = sqrtf(a[0] * a[0] + a[1] * a[1]);
    a[0] /= al;
    a[1] /= al;
    const float b[3] = {n[1] * a[2] - n[2] * a[1], n[2] * a[0] - n[0] * a[2], n[0] * a[1] - n[1] * a[0]};
    for (int i = 0; i < SKY_STARS; i++) {
        sky_star *st = &out[i];
        if (i < SKY_STARS * 35 / 100) {
            /* In the band: around the plane, spread by about 7 degrees
             * (the sum of three uniforms is close to a normal). */
            float ang = 2.0f * SKY_PI * rnd(&s);
            float lat = 0.12f * (rnd(&s) + rnd(&s) + rnd(&s) - 1.5f) * 2.0f;
            float c = cosf(lat), sn = sinf(lat);
            for (int k = 0; k < 3; k++) st->dir[k] = (a[k] * cosf(ang) + b[k] * sinf(ang)) * c + n[k] * sn;
        } else {
            /* Uniform over the sphere. */
            float z = 2.0f * rnd(&s) - 1.0f, ang = 2.0f * SKY_PI * rnd(&s), r = sqrtf(1.0f - z * z);
            st->dir[0] = r * cosf(ang);
            st->dir[1] = z;
            st->dir[2] = r * sinf(ang);
        }
        /* Brightness falls steeply with magnitude: most stars are faint,
         * a handful bright and a little larger. */
        float m = rnd(&s), bright = 0.12f + 0.88f * m * m * m * m * m * m;
        st->size = 0.8f + 1.2f * m * m * m * m;
        star_colour(rnd(&s), st->rgb);
        for (int k = 0; k < 3; k++) st->rgb[k] *= bright;
        st->phase = rnd(&s);
    }
}

quat sky_rotation(double day)
{
    /* The pole: 40 degrees up toward -z (north); the sun circles it, from
     * +x at 06:00 to the south at noon, which about this axis is a turn of
     * minus the hour angle. */
    float ang = (float)(-2.0 * 3.14159265358979 * (day - floor(day)));
    return quat_axis(0.0f, 0.6428f, -0.7660f, ang);
}

float sky_visibility(float daylight)
{
    float day = (daylight - THERMO_NIGHT_LIGHT) / (1.0f - THERMO_NIGHT_LIGHT);
    float t = day < 0.0f ? 0.0f : (day > 0.3f ? 1.0f : day / 0.3f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

sky_star sky_moon(const float sun_dir[3])
{
    /* Drawn 3 degrees across (a 1080-line screen at 70 degrees: 46
     * pixels), six times the real moon, so it reads at this resolution. */
    sky_star m = {{-sun_dir[0], -sun_dir[1], -sun_dir[2]}, 23.0f, {0.85f, 0.87f, 0.9f}, -1.0f};
    return m;
}
