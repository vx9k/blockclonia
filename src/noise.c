/* No fused multiply-adds, so a seed's noise is the same on every machine
 * (see worldgen.c), and the AVX2 path rounds exactly as the plain one. */
#pragma STDC FP_CONTRACT OFF
#include "noise.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

/* Simplex noise: the plane cut into triangles (space into tetrahedra), so
 * a point blends 3 corners in 2D and 4 in 3D where lattice noise blends 4
 * and 8, and the field has no grain along the axes. */
/* A simplex sum is flatter in the middle than lattice noise and stops
 * short of its tails: scaled to the same spread it would never pass 0.56,
 * and every threshold the terrain was tuned on above that (lakes, cave
 * chambers) would never be met. So each sample goes through an odd
 * polynomial fitted to give the old lattice noise's distribution, quantile
 * for quantile, within 0.01 up to the 99.9th percentile. U brings the sum
 * to [-1, 1] before it. */
#define U2 45.231f
#define U3 32.762f
static const float SHAPE2[4] = {0.410741f, 0.192520f, 0.149835f, 0.103433f};
static const float SHAPE3[4] = {0.583895f, 0.073913f, -0.034395f, 0.234758f};
/* How much a steep octave damps the finer ones in eroded2d: 1 / (1 + E
 * |slope|^2). The shaped slopes have a longer tail than the lattice
 * noise's, so no E gives its whole spread back; this one keeps the share
 * of ground over 60 m and over 100 m above the sea within 0.2 points. */
#define EROSION 0.55f
/* Simplex cells are smaller than lattice cells, so the input is slowed to
 * keep each feature as wide as before (the same mean slope for the same
 * spread): hills sized "90 blocks across" still are. */
#define K2 0.5638
#define K3 0.6443

/* The skew into the lattice of triangles, and back. The skew is done in
 * double, so the offset into a cell is exact far from the origin. */
#define F2 0.36602540378443864676 /* (sqrt(3) - 1) / 2 */
#define G2 0.21132486540518711775f /* (3 - sqrt(3)) / 6 */
#define F3 (1.0 / 3.0)
#define G3 (1.0f / 6.0f)

/* The kernels go whole into each batch loop, where the vectoriser can see
 * them; clang's cost model would otherwise keep them as calls. */
#define KERNEL static inline __attribute__((always_inline))

/* A corner's hash: the cell, counted modulo 2^32 so the octaves of fbm2d
 * can run past INT_MAX near the world's edge, spread by odd constants and
 * mixed once with the seed, which the caller has mixed already. Only the
 * low four bits pick a gradient. The caller passes each coordinate times
 * its constant: a cell's neighbour is one constant more, an add where a
 * multiply would be the dearest step on small cores. */
#define KX 0x9E3779B1U
#define KY 0xC2B2AE3DU
#define KZ 0x85EBCA77U
KERNEL uint32_t corner_hash(uint32_t seed, uint32_t xk, uint32_t yk, uint32_t zk)
{
    return hash_u32(seed ^ xk ^ yk ^ zk);
}

/* A lattice coordinate, already floored, counted modulo 2^32: the low bits
 * of its two's complement. Adding 1.5 * 2^52 moves the integer into the
 * mantissa's low bits, exactly while |fl| < 2^51 (2e9 blocks at 12
 * octaves is 2^42 cells): one add and a move where a conversion to a
 * 64-bit integer has no SIMD form before AVX-512. */
KERNEL uint32_t cell_u32(double fl)
{
    double m = fl + 6755399441055744.0;
    uint64_t bits = 0;
    memcpy(&bits, &m, sizeof bits);
    return (uint32_t)bits;
}

/* One corner's share: (0.5 - r^2)^4 times its gradient's dot product with
 * the offset (x, z), and that share's slope. The gradients are the eight
 * (+-1, +-2) and (+-2, +-1), picked from the hash with selects rather than
 * a table, so a batch of corners vectorises without gathers. */
KERNEL float corner2(uint32_t h, float x, float z, float *dx, float *dz)
{
    float t = 0.5f - x * x - z * z;
    t = t > 0.0f ? t : 0.0f;
    float a = (h & 1u) ? -1.0f : 1.0f, b = (h & 2u) ? -2.0f : 2.0f;
    float gx = (h & 4u) ? b : a, gz = (h & 4u) ? a : b;
    float dot = gx * x + gz * z, t2 = t * t, t4 = t2 * t2, k = 8.0f * t2 * t * dot;
    *dx = t4 * gx - k * x;
    *dz = t4 * gz - k * z;
    return t4 * dot;
}

/* The distribution's shape at u, u * (c0 + c1 u^2 + c2 u^8 + c3 u^20), and
 * its slope. */
KERNEL float shape(const float *c, float u, float *slope)
{
    float w = u * u, w2 = w * w, w4 = w2 * w2, w10 = (w4 * w4) * w2;
    *slope = c[0] + 3.0f * c[1] * w + 9.0f * c[2] * w4 + 21.0f * c[3] * w10;
    return u * (c[0] + c[1] * w + c[2] * w4 + c[3] * w10);
}

/* noise2d and its gradient at one point: the plain and AVX2 batches and
 * the single-point calls all run this, so they agree to the last bit. */
KERNEL float simplex2(uint32_t seed, double x, double z, float *dx, float *dz)
{
    seed = hash_u32(seed + 0x6A09E667U);
    x *= K2;
    z *= K2;
    double s = (x + z) * F2, xs = x + s, zs = z + s;
    double fi = floor(xs), fj = floor(zs);
    uint32_t i = cell_u32(fi), j = cell_u32(fj);
    float xr = (float)(xs - fi), zr = (float)(zs - fj), t = (xr + zr) * G2;
    float x0 = xr - t, z0 = zr - t;
    /* The lower or the upper triangle of the cell. */
    uint32_t up = x0 > z0 ? 1u : 0u;
    float i1 = x0 > z0 ? 1.0f : 0.0f, j1 = 1.0f - i1;
    float x1 = x0 - i1 + G2, z1 = z0 - j1 + G2;
    float x2 = x0 - 1.0f + 2.0f * G2, z2 = z0 - 1.0f + 2.0f * G2;
    float ax = 0.0f, az = 0.0f, bx = 0.0f, bz = 0.0f, cx = 0.0f, cz = 0.0f;
    uint32_t ik = i * KX, jk = j * KZ;
    float n = corner2(corner_hash(seed, ik, 0u, jk), x0, z0, &ax, &az) +
              corner2(corner_hash(seed, ik + (up ? KX : 0u), 0u, jk + (up ? 0u : KZ)), x1, z1, &bx, &bz) +
              corner2(corner_hash(seed, ik + KX, 0u, jk + KZ), x2, z2, &cx, &cz);
    float slope = 0.0f, v = shape(SHAPE2, n * U2, &slope), g = slope * (U2 * (float)K2);
    *dx = (ax + bx + cx) * g;
    *dz = (az + bz + cz) * g;
    return v;
}

/* The 3D corner: (0.6 - r^2)^4 times the dot product with one of the
 * twelve cube-edge directions (sixteen picks, four of them twice). */
KERNEL float corner3(uint32_t h, float x, float y, float z)
{
    float t = 0.6f - x * x - y * y - z * z;
    t = t > 0.0f ? t : 0.0f;
    uint32_t g = h & 15u;
    float u = g < 8u ? x : y, v = g < 4u ? y : (g == 12u || g == 14u ? x : z);
    float dot = ((g & 1u) ? -u : u) + ((g & 2u) ? -v : v), t2 = t * t;
    return t2 * t2 * dot;
}

KERNEL float simplex3(uint32_t seed, double x, double y, double z)
{
    seed = hash_u32(seed + 0xBB67AE85U);
    x *= K3;
    y *= K3;
    z *= K3;
    double s = (x + y + z) * F3, xs = x + s, ys = y + s, zs = z + s;
    double fi = floor(xs), fj = floor(ys), fk = floor(zs);
    uint32_t i = cell_u32(fi), j = cell_u32(fj), k = cell_u32(fk);
    float xr = (float)(xs - fi), yr = (float)(ys - fj), zr = (float)(zs - fk), t = (xr + yr + zr) * G3;
    float x0 = xr - t, y0 = yr - t, z0 = zr - t;
    /* The tetrahedron: step first along the largest offset, then along the
     * next. The ranks are a permutation of 0, 1, 2 even where offsets tie. */
    uint32_t rx = (x0 >= y0 ? 1u : 0u) + (x0 >= z0 ? 1u : 0u);
    uint32_t ry = (y0 > x0 ? 1u : 0u) + (y0 >= z0 ? 1u : 0u);
    uint32_t rz = (z0 > x0 ? 1u : 0u) + (z0 > y0 ? 1u : 0u);
    uint32_t i1 = rx >= 2u ? 1u : 0u, j1 = ry >= 2u ? 1u : 0u, k1 = rz >= 2u ? 1u : 0u;
    uint32_t i2 = rx >= 1u ? 1u : 0u, j2 = ry >= 1u ? 1u : 0u, k2 = rz >= 1u ? 1u : 0u;
    uint32_t ik = i * KX, jk = j * KY, kk = k * KZ;
    float n = corner3(corner_hash(seed, ik, jk, kk), x0, y0, z0) +
              corner3(corner_hash(seed, ik + (i1 ? KX : 0u), jk + (j1 ? KY : 0u), kk + (k1 ? KZ : 0u)),
                      x0 - (float)i1 + G3, y0 - (float)j1 + G3, z0 - (float)k1 + G3) +
              corner3(corner_hash(seed, ik + (i2 ? KX : 0u), jk + (j2 ? KY : 0u), kk + (k2 ? KZ : 0u)),
                      x0 - (float)i2 + 2.0f * G3, y0 - (float)j2 + 2.0f * G3, z0 - (float)k2 + 2.0f * G3) +
              corner3(corner_hash(seed, ik + KX, jk + KY, kk + KZ), x0 - 1.0f + 3.0f * G3, y0 - 1.0f + 3.0f * G3,
                      z0 - 1.0f + 3.0f * G3);
    float slope = 0.0f;
    return shape(SHAPE3, n * U3, &slope);
}

/* ------------------------------------------------------------ batches */

/* The batch loops, compiled twice on x86_64: for the build's own target
 * (SSE2, or SSE4.2 in the packages) and for AVX2, chosen at run time.
 * Builds that already target AVX2, and arm64 (NEON), have one copy. */
#define BATCH2(name, attr)                                                                                          \
    attr static void name(uint32_t seed, const double *x, const double *z, float *out, float *dx, float *dz, int n) \
    {                                                                                                               \
        for (int i = 0; i < n; i++) out[i] = simplex2(seed, x[i], z[i], &dx[i], &dz[i]);                            \
    }
#define BATCH3(name, attr)                                                                                     \
    attr static void name(uint32_t seed, const double *x, const double *y, const double *z, float *out, int n) \
    {                                                                                                          \
        for (int i = 0; i < n; i++) out[i] = simplex3(seed, x[i], y[i], z[i]);                                 \
    }

BATCH2(batch2_plain, )
BATCH3(batch3_plain, )

#if defined(__x86_64__) && !defined(__AVX2__)
#define NOISE_DISPATCH 1
BATCH2(batch2_avx2, __attribute__((target("avx2"))))
BATCH3(batch3_avx2, __attribute__((target("avx2"))))
#endif

/* 1 to use AVX2 where the CPU has it, 0 not to; -1 before the first look. */
static atomic_int g_avx2 = -1;
static atomic_int g_avx2_allowed = 1;

static int use_avx2(void)
{
    int v = atomic_load_explicit(&g_avx2, memory_order_relaxed);
    if (v >= 0) return v;
#ifdef NOISE_DISPATCH
    __builtin_cpu_init();
    v = __builtin_cpu_supports("avx2") && atomic_load_explicit(&g_avx2_allowed, memory_order_relaxed);
#else
    v = 0;
#endif
    atomic_store_explicit(&g_avx2, v, memory_order_relaxed);
    return v;
}

int noise_avx2(int allow)
{
    atomic_store_explicit(&g_avx2_allowed, allow, memory_order_relaxed);
    atomic_store_explicit(&g_avx2, -1, memory_order_relaxed);
    return use_avx2();
}

/* Batches go through in blocks of this many points, with their scratch on
 * the stack. */
#define BLOCK 64

static void batch2(uint32_t seed, const double *x, const double *z, float *out, float *dx, float *dz, int n)
{
#ifdef NOISE_DISPATCH
    if (use_avx2()) {
        batch2_avx2(seed, x, z, out, dx, dz, n);
        return;
    }
#endif
    batch2_plain(seed, x, z, out, dx, dz, n);
}

void noise2d_grad_n(uint32_t seed, const double *x, const double *z, float *out, float *dx, float *dz, int n)
{
    for (int b = 0; b < n; b += BLOCK) {
        int m = n - b < BLOCK ? n - b : BLOCK;
        float gx[BLOCK], gz[BLOCK]; /* scratch, written by batch2 before any read */
        batch2(seed, x + b, z + b, out + b, dx ? dx + b : gx, dz ? dz + b : gz, m);
    }
}

void noise2d_n(uint32_t seed, const double *x, const double *z, float *out, int n)
{
    noise2d_grad_n(seed, x, z, out, NULL, NULL, n);
}

void noise3d_n(uint32_t seed, const double *x, const double *y, const double *z, float *out, int n)
{
#ifdef NOISE_DISPATCH
    if (use_avx2()) {
        batch3_avx2(seed, x, y, z, out, n);
        return;
    }
#endif
    batch3_plain(seed, x, y, z, out, n);
}

/* Octaves at double the frequency and half the amplitude; doubling a
 * double is exact. The sums run in the same order as fbm2d's. */
void fbm2d_n(uint32_t seed, const double *x, const double *z, int octaves, float *out, int n)
{
    for (int b = 0; b < n; b += BLOCK) {
        int m = n - b < BLOCK ? n - b : BLOCK;
        double px[BLOCK], pz[BLOCK];
        float sum[BLOCK], v[BLOCK], gx[BLOCK], gz[BLOCK]; /* v, gx, gz: written by batch2 first */
        for (int i = 0; i < m; i++) {
            px[i] = x[b + i];
            pz[i] = z[b + i];
            sum[i] = 0.0f;
        }
        float amp = 1.0f, norm = 0.0f;
        for (int o = 0; o < octaves; o++) {
            batch2(seed + (uint32_t)o * 1013U, px, pz, v, gx, gz, m);
            for (int i = 0; i < m; i++) {
                sum[i] += amp * v[i];
                px[i] *= 2.0;
                pz[i] *= 2.0;
            }
            norm += amp;
            amp *= 0.5f;
        }
        for (int i = 0; i < m; i++) out[b + i] = sum[i] / norm;
    }
}

/* Each octave is damped by the slope summed over the octaves before it
 * and its own: amplitude times frequency is 1 for every octave, so the
 * gradients add as they come. */
void eroded2d_n(uint32_t seed, const double *x, const double *z, int octaves, float *out, int n)
{
    for (int b = 0; b < n; b += BLOCK) {
        int m = n - b < BLOCK ? n - b : BLOCK;
        double px[BLOCK], pz[BLOCK];
        float sum[BLOCK], sx[BLOCK], sz[BLOCK], v[BLOCK], gx[BLOCK], gz[BLOCK]; /* v, gx, gz: written first */
        for (int i = 0; i < m; i++) {
            px[i] = x[b + i];
            pz[i] = z[b + i];
            sum[i] = sx[i] = sz[i] = 0.0f;
        }
        float amp = 1.0f, norm = 0.0f;
        for (int o = 0; o < octaves; o++) {
            batch2(seed + (uint32_t)o * 1013U, px, pz, v, gx, gz, m);
            for (int i = 0; i < m; i++) {
                sx[i] += gx[i];
                sz[i] += gz[i];
                sum[i] += amp * v[i] / (1.0f + EROSION * (sx[i] * sx[i] + sz[i] * sz[i]));
                px[i] *= 2.0;
                pz[i] *= 2.0;
            }
            norm += amp;
            amp *= 0.5f;
        }
        for (int i = 0; i < m; i++) out[b + i] = sum[i] / norm;
    }
}

/* ------------------------------------------------------- single points */

float noise2d(uint32_t seed, double x, double z)
{
    float dx = 0.0f, dz = 0.0f;
    return simplex2(seed, x, z, &dx, &dz);
}

float noise2d_grad(uint32_t seed, double x, double z, float *dx, float *dz) { return simplex2(seed, x, z, dx, dz); }

float noise3d(uint32_t seed, double x, double y, double z) { return simplex3(seed, x, y, z); }

/* The same steps as fbm2d_n and eroded2d_n, in the same order, for one
 * point without the batch's blocks. */
float fbm2d(uint32_t seed, double x, double z, int octaves)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, dx = 0.0f, dz = 0.0f;
    for (int o = 0; o < octaves; o++) {
        sum += amp * simplex2(seed + (uint32_t)o * 1013U, x, z, &dx, &dz);
        x *= 2.0;
        z *= 2.0;
        norm += amp;
        amp *= 0.5f;
    }
    return sum / norm;
}

float eroded2d(uint32_t seed, double x, double z, int octaves)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, sx = 0.0f, sz = 0.0f;
    for (int o = 0; o < octaves; o++) {
        float gx = 0.0f, gz = 0.0f;
        float v = simplex2(seed + (uint32_t)o * 1013U, x, z, &gx, &gz);
        sx += gx;
        sz += gz;
        sum += amp * v / (1.0f + EROSION * (sx * sx + sz * sz));
        x *= 2.0;
        z *= 2.0;
        norm += amp;
        amp *= 0.5f;
    }
    return sum / norm;
}
