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

float noise2(uint32_t seed, float x, float z);                  /* [-1, 1] */
float fbm2(uint32_t seed, float x, float z, int octaves);       /* ~[-1, 1] */
float noise3(uint32_t seed, float x, float y, float z);         /* ~[-1, 1] */

/* The same noise at block coordinates divided by a scale (noise2i(s, x, z,
 * 90) is noise2(s, x / 90, z / 90)), for any int coordinate. A float holds
 * a coordinate near 2e9 only to the nearest 128 blocks; here the quotient
 * is split into lattice cell and fraction in double, good to a few
 * micrometres, before the float arithmetic. The results differ from the
 * float versions in the last bits, so generators keep those where worlds
 * were made with them. The lattice cell must fit an int: |x| / scale *
 * 2^(octaves - 1) < 2^31. */
float noise2i(uint32_t seed, int x, int z, double scale);
float fbm2i(uint32_t seed, int x, int z, double scale, int octaves);
float noise3i(uint32_t seed, int x, int y, int z, double scale_xz, double scale_y);

#endif
