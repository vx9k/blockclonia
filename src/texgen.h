/* Procedural 16x16 block textures, generated at startup so the game ships
 * no image files (nothing to parse, nothing to load from disk). */
#ifndef MC_TEXGEN_H
#define MC_TEXGEN_H

#include <stdint.h>

#define TEX_SIZE 16
#define TEX_MIPS 5 /* 16, 8, 4, 2, 1 */

/* Total RGBA8 bytes for all layers and mips. */
uint32_t texgen_total_bytes(void);
/* Byte offset of (layer, mip) inside the buffer texgen_build fills. Mips are
 * stored mip-major: all layers of mip 0, then all of mip 1, ... */
uint32_t texgen_offset(int layer, int mip);
void texgen_build(uint8_t *out);

#endif
