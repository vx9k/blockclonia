#include "noise.h"
#include <math.h>

static const float GRAD[8][2] = {
    {1, 0}, {-1, 0}, {0, 1}, {0, -1},
    {0.70710678f, 0.70710678f}, {-0.70710678f, 0.70710678f},
    {0.70710678f, -0.70710678f}, {-0.70710678f, -0.70710678f},
};

static inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

static inline float grad_dot(uint32_t seed, int ix, int iz, float fx, float fz)
{
    const float *g = GRAD[hash2i(seed, ix, iz) & 7];
    return g[0] * fx + g[1] * fz;
}

float noise2(uint32_t seed, float x, float z)
{
    float flx = floorf(x), flz = floorf(z);
    int ix = (int)flx, iz = (int)flz;
    float fx = x - flx, fz = z - flz;
    float u = fade(fx), v = fade(fz);
    float n00 = grad_dot(seed, ix, iz, fx, fz);
    float n10 = grad_dot(seed, ix + 1, iz, fx - 1.0f, fz);
    float n01 = grad_dot(seed, ix, iz + 1, fx, fz - 1.0f);
    float n11 = grad_dot(seed, ix + 1, iz + 1, fx - 1.0f, fz - 1.0f);
    float nx0 = n00 + u * (n10 - n00);
    float nx1 = n01 + u * (n11 - n01);
    /* Scale so the output roughly spans [-1, 1]. */
    return (nx0 + v * (nx1 - nx0)) * 1.41421356f;
}

float fbm2(uint32_t seed, float x, float z, int octaves)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; i++) {
        sum += amp * noise2(seed + (uint32_t)i * 1013U, x, z);
        norm += amp;
        amp *= 0.5f;
        x *= 2.0f;
        z *= 2.0f;
    }
    return sum / norm;
}
