/* Deterministic, seedable 2D and 3D gradient noise. No global state, so worker
 * threads can call it freely. */
#ifndef MC_NOISE_H
#define MC_NOISE_H

#include <stdint.h>

static inline uint32_t hash_u32(uint32_t h)
{
    h ^= h >> 16; h *= 0x7feb352dU;
    h ^= h >> 15; h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

static inline uint32_t hash2i(uint32_t seed, int x, int z)
{
    return hash_u32(seed ^ hash_u32((uint32_t)x * 0x9E3779B1U ^ hash_u32((uint32_t)z + 0x632BE5ABU)));
}

/* Coordinates are block positions divided by the feature size, in double
 * (noise2d(s, x / 90.0, z / 90.0) has hills ~90 blocks across), so they
 * stay exact out to the world's edge, at any octave. */
float noise2d(uint32_t seed, double x, double z);            /* [-1, 1] */
float fbm2d(uint32_t seed, double x, double z, int octaves); /* ~[-1, 1] */
float noise3d(uint32_t seed, double x, double y, double z);  /* ~[-1, 1] */
/* noise2d and its gradient, d/dx and d/dz per unit of x and z. */
float noise2d_grad(uint32_t seed, double x, double z, float *dx, float *dz);
/* fbm whose finer octaves fade where the coarser ones are already steep,
 * the way erosion leaves sharp peaks over smooth valleys (instead of
 * ridged noise's thin worms). */
float eroded2d(uint32_t seed, double x, double z, int octaves);

#endif
