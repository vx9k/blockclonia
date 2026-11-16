/* No fused multiply-adds, so a seed's noise is the same on every machine
 * (see worldgen.c). */
#pragma STDC FP_CONTRACT OFF
#include "noise.h"
#include <math.h>

static const float GRAD[8][2] = {
    {1, 0}, {-1, 0}, {0, 1}, {0, -1},
    {0.70710678f, 0.70710678f}, {-0.70710678f, 0.70710678f},
    {0.70710678f, -0.70710678f}, {-0.70710678f, -0.70710678f},
};

static inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

/* hash2i on lattice cells counted modulo 2^32: the octaves of fbm2d run
 * past INT_MAX near the world's edge, and wrapping is free there since
 * hash2i only ever looks at the low 32 bits. */
static inline uint32_t hash2u(uint32_t seed, uint32_t x, uint32_t z)
{
    return hash_u32(seed ^ hash_u32(x * 0x9E3779B1U ^ hash_u32(z + 0x632BE5ABU)));
}

static inline float grad_dot(uint32_t seed, uint32_t ix, uint32_t iz, float fx, float fz)
{
    const float *g = GRAD[hash2u(seed, ix, iz) & 7];
    return g[0] * fx + g[1] * fz;
}

/* Lattice cell (ix, iz) and the offset into it, 0 <= f < 1. */
static float noise2_cell(uint32_t seed, uint32_t ix, uint32_t iz, float fx, float fz)
{
    float u = fade(fx), v = fade(fz);
    float n00 = grad_dot(seed, ix, iz, fx, fz);
    float n10 = grad_dot(seed, ix + 1U, iz, fx - 1.0f, fz);
    float n01 = grad_dot(seed, ix, iz + 1U, fx, fz - 1.0f);
    float n11 = grad_dot(seed, ix + 1U, iz + 1U, fx - 1.0f, fz - 1.0f);
    float nx0 = n00 + u * (n10 - n00);
    float nx1 = n01 + u * (n11 - n01);
    /* Scale so the output roughly spans [-1, 1]. */
    return (nx0 + v * (nx1 - nx0)) * 1.41421356f;
}

/* Splits a lattice coordinate into its cell and the offset into it. x -
 * floor(x) is exact in binary floating point, so only the fraction's
 * rounding to float, 6e-8 of a cell, is lost, even 2e9 blocks out where a
 * float alone would hold a coordinate only to the nearest 128 blocks. A
 * fraction rounded up to 1 is harmless: the noise is continuous across
 * cells. The cell is counted modulo 2^32 (exact in double), the same bits
 * an int cast gives wherever it fits. */
static uint32_t cell_of(double x, float *frac)
{
    double fl = floor(x);
    *frac = (float)(x - fl);
    return (uint32_t)(fl - 4294967296.0 * floor(fl / 4294967296.0));
}

float noise2d(uint32_t seed, double x, double z)
{
    float fx = 0.0f, fz = 0.0f;
    uint32_t ix = cell_of(x, &fx), iz = cell_of(z, &fz);
    return noise2_cell(seed, ix, iz, fx, fz);
}

/* Octaves at double the frequency and half the amplitude; doubling a
 * double is exact. */
float fbm2d(uint32_t seed, double x, double z, int octaves)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; i++) {
        sum += amp * noise2d(seed + (uint32_t)i * 1013U, x, z);
        norm += amp;
        amp *= 0.5f;
        x *= 2.0;
        z *= 2.0;
    }
    return sum / norm;
}

static inline float fade_d(float t) { return 30.0f * t * t * (t - 1.0f) * (t - 1.0f); }

/* noise2_cell differentiated: each corner's dot product is linear in the
 * offset, and the fade curves blend them. */
float noise2d_grad(uint32_t seed, double x, double z, float *dx, float *dz)
{
    float fx = 0.0f, fz = 0.0f;
    uint32_t ix = cell_of(x, &fx), iz = cell_of(z, &fz);
    const float *g00 = GRAD[hash2u(seed, ix, iz) & 7], *g10 = GRAD[hash2u(seed, ix + 1U, iz) & 7];
    const float *g01 = GRAD[hash2u(seed, ix, iz + 1U) & 7], *g11 = GRAD[hash2u(seed, ix + 1U, iz + 1U) & 7];
    float u = fade(fx), v = fade(fz), du = fade_d(fx), dv = fade_d(fz);
    float n00 = g00[0] * fx + g00[1] * fz, n10 = g10[0] * (fx - 1.0f) + g10[1] * fz;
    float n01 = g01[0] * fx + g01[1] * (fz - 1.0f), n11 = g11[0] * (fx - 1.0f) + g11[1] * (fz - 1.0f);
    float nx0 = n00 + u * (n10 - n00), nx1 = n01 + u * (n11 - n01);
    float ax0 = g00[0] + u * (g10[0] - g00[0]) + du * (n10 - n00);
    float ax1 = g01[0] + u * (g11[0] - g01[0]) + du * (n11 - n01);
    float az0 = g00[1] + u * (g10[1] - g00[1]), az1 = g01[1] + u * (g11[1] - g01[1]);
    *dx = (ax0 + v * (ax1 - ax0)) * 1.41421356f;
    *dz = (az0 + v * (az1 - az0) + dv * (nx1 - nx0)) * 1.41421356f;
    return (nx0 + v * (nx1 - nx0)) * 1.41421356f;
}

/* Each octave is damped by the slope summed over the octaves before it
 * and its own: amplitude times frequency is 1 for every octave, so the
 * gradients add as they come. */
float eroded2d(uint32_t seed, double x, double z, int octaves)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, sx = 0.0f, sz = 0.0f;
    for (int i = 0; i < octaves; i++) {
        float gx = 0.0f, gz = 0.0f;
        float n = noise2d_grad(seed + (uint32_t)i * 1013U, x, z, &gx, &gz);
        sx += gx;
        sz += gz;
        sum += amp * n / (1.0f + 0.6f * (sx * sx + sz * sz));
        norm += amp;
        amp *= 0.5f;
        x *= 2.0;
        z *= 2.0;
    }
    return sum / norm;
}

/* The 12 cube-edge directions of improved Perlin noise. */
static const float GRAD3[12][3] = {
    {1, 1, 0},  {-1, 1, 0},  {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
    {1, 0, -1}, {-1, 0, -1}, {0, 1, 1},  {0, -1, 1},  {0, 1, -1}, {0, -1, -1},
};

static inline float grad3_dot(uint32_t seed, uint32_t ix, uint32_t iy, uint32_t iz, float fx, float fy, float fz)
{
    const float *g = GRAD3[hash2u(seed ^ hash_u32(iy * 0x85EBCA77U), ix, iz) % 12u];
    return g[0] * fx + g[1] * fy + g[2] * fz;
}

static float noise3_cell(uint32_t seed, uint32_t ix, uint32_t iy, uint32_t iz, float fx, float fy, float fz)
{
    float u = fade(fx), v = fade(fy), w = fade(fz);
    float c[2][2] = {{0}};
    for (uint32_t dy = 0; dy < 2; dy++)
        for (uint32_t dz = 0; dz < 2; dz++) {
            float a = grad3_dot(seed, ix, iy + dy, iz + dz, fx, fy - (float)dy, fz - (float)dz);
            float b = grad3_dot(seed, ix + 1U, iy + dy, iz + dz, fx - 1.0f, fy - (float)dy, fz - (float)dz);
            c[dy][dz] = a + u * (b - a);
        }
    float y0 = c[0][0] + w * (c[0][1] - c[0][0]);
    float y1 = c[1][0] + w * (c[1][1] - c[1][0]);
    return y0 + v * (y1 - y0);
}

float noise3d(uint32_t seed, double x, double y, double z)
{
    float fx = 0.0f, fy = 0.0f, fz = 0.0f;
    uint32_t ix = cell_of(x, &fx), iy = cell_of(y, &fy), iz = cell_of(z, &fz);
    return noise3_cell(seed, ix, iy, iz, fx, fy, fz);
}
