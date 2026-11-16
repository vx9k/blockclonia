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

/* Lattice cell (ix, iz) and the offset into it, 0 <= f < 1. */
static float noise2_cell(uint32_t seed, int ix, int iz, float fx, float fz)
{
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

float noise2(uint32_t seed, float x, float z)
{
    float flx = floorf(x), flz = floorf(z);
    return noise2_cell(seed, (int)flx, (int)flz, x - flx, z - flz);
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

/* x - floor(x) is exact in binary floating point, so beyond the quotient's
 * own rounding (1e-16 of it) only the fraction's to float, 6e-8 of a cell,
 * is lost. A fraction rounded up to 1 is harmless: the noise is continuous
 * across cells. */
static int cell_of(double x, float *frac)
{
    double fl = floor(x);
    *frac = (float)(x - fl);
    return (int)fl;
}

float noise2i(uint32_t seed, int x, int z, double scale)
{
    float fx = 0.0f, fz = 0.0f;
    int ix = cell_of((double)x / scale, &fx), iz = cell_of((double)z / scale, &fz);
    return noise2_cell(seed, ix, iz, fx, fz);
}

/* fbm2's octaves; doubling a double is exact, as doubling the float was. */
float fbm2i(uint32_t seed, int x, int z, double scale, int octaves)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    double px = (double)x / scale, pz = (double)z / scale;
    for (int i = 0; i < octaves; i++) {
        float fx = 0.0f, fz = 0.0f;
        int ix = cell_of(px, &fx), iz = cell_of(pz, &fz);
        sum += amp * noise2_cell(seed + (uint32_t)i * 1013U, ix, iz, fx, fz);
        norm += amp;
        amp *= 0.5f;
        px *= 2.0;
        pz *= 2.0;
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

static float noise3_cell(uint32_t seed, int ix, int iy, int iz, float fx, float fy, float fz)
{
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

float noise3(uint32_t seed, float x, float y, float z)
{
    float flx = floorf(x), fly = floorf(y), flz = floorf(z);
    return noise3_cell(seed, (int)flx, (int)fly, (int)flz, x - flx, y - fly, z - flz);
}

float noise3i(uint32_t seed, int x, int y, int z, double scale_xz, double scale_y)
{
    float fx = 0.0f, fy = 0.0f, fz = 0.0f;
    int ix = cell_of((double)x / scale_xz, &fx), iy = cell_of((double)y / scale_y, &fy),
        iz = cell_of((double)z / scale_xz, &fz);
    return noise3_cell(seed, ix, iy, iz, fx, fy, fz);
}
