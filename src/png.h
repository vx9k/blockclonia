/* PNG encoder for screenshots: 8-bit RGB in, a complete PNG file out.
 * Rows are filtered (the PNG filter that leaves the smallest residuals) and
 * deflated by zlib-ng (or the stock zlib, where only that is packaged).
 * It does not decode, write palettes, alpha or
 * interlacing, or split the image over several IDAT chunks. */
#ifndef MC_PNG_H
#define MC_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Encodes w x h pixels of packed RGB (3 bytes each, rows top to bottom)
 * into a buffer from mem_alloc that the caller frees with mem_free.
 * Returns the PNG's length, or 0 when w or h is 0 or the image is too
 * large for one PNG chunk. */
size_t png_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t **out);

#endif
