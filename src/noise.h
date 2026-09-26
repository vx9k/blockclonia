/* Deterministic, seedable 2D gradient noise. No global state, so worker
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

#endif
