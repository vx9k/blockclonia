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

/* The 12 cube-edge directions of improved Perlin noise. */
static const float GRAD3[12][3] = {
    {1, 1, 0},  {-1, 1, 0},  {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
    {1, 0, -1}, {-1, 0, -1}, {0, 1, 1},  {0, -1, 1},  {0, 1, -1}, {0, -1, -1},
};

static inline float grad3_dot(uint32_t seed, int ix, int iy, int iz, float fx, float fy, float fz)
{
    const float *g = GRAD3[hash2i(seed ^ hash_u32((uint32_t)iy * 0x85EBCA77U), ix, iz) % 12u];
    return g[0] * fx + g[1] * fy + g[2] * fz;
}

float noise3(uint32_t seed, float x, float y, float z)
{
    float flx = floorf(x), fly = floorf(y), flz = floorf(z);
    int ix = (int)flx, iy = (int)fly, iz = (int)flz;
    float fx = x - flx, fy = y - fly, fz = z - flz;
    float u = fade(fx), v = fade(fy), w = fade(fz);
    float c[2][2] = {{0}};
    for (int dy = 0; dy < 2; dy++)
        for (int dz = 0; dz < 2; dz++) {
            float a = grad3_dot(seed, ix, iy + dy, iz + dz, fx, fy - (float)dy, fz - (float)dz);
            float b = grad3_dot(seed, ix + 1, iy + dy, iz + dz, fx - 1.0f, fy - (float)dy, fz - (float)dz);
            c[dy][dz] = a + u * (b - a);
        }
    float y0 = c[0][0] + w * (c[0][1] - c[0][0]);
    float y1 = c[1][0] + w * (c[1][1] - c[1][0]);
    return y0 + v * (y1 - y0);
}
