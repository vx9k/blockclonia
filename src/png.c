#include "png.h"
#include "mem.h"

#include <string.h>
#ifdef MC_ZLIBNG
#include <zlib-ng.h>
#else
#include <zlib.h>
#endif

/* On a 1080p game frame: level 1 takes 28 ms for 1.27 MB, 3 takes 42 ms
 * for 0.82 MB, 6 takes 95 ms for 0.78 MB and 9 takes 515 ms for 0.75 MB.
 * The encode runs on the main thread when F2 is pressed, so 3. */
#define PNG_LEVEL 3

/* ---- PNG filters (ISO 15948, 9.2) */

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
}

/* Filters row y with filter f into out (without the filter byte) and
 * returns the sum of the residuals as signed bytes: the usual guess at
 * which filter compresses best. Filters 2 to 4 read the row above, so
 * they are only asked for from the second row on. */
static uint32_t filter_row(const uint8_t *rgb, size_t stride, uint32_t y, int f, uint8_t *out)
{
    const uint8_t *row = rgb + (size_t)y * stride, *up = row - stride;
    /* One loop per filter, so the choice is not made per byte. The first
     * pixel has no left neighbour (a = c = 0). */
    for (size_t i = 0; i < 3; i++) {
        int b = f >= 2 ? up[i] : 0;
        out[i] = (uint8_t)(row[i] - (f == 3 ? b / 2 : b));
    }
    switch (f) {
    case 0: memcpy(out + 3, row + 3, stride - 3); break;
    case 1:
        for (size_t i = 3; i < stride; i++) out[i] = (uint8_t)(row[i] - row[i - 3]);
        break;
    case 2:
        for (size_t i = 3; i < stride; i++) out[i] = (uint8_t)(row[i] - up[i]);
        break;
    case 3:
        for (size_t i = 3; i < stride; i++) out[i] = (uint8_t)(row[i] - (row[i - 3] + up[i]) / 2);
        break;
    default:
        for (size_t i = 3; i < stride; i++) out[i] = (uint8_t)(row[i] - paeth(row[i - 3], up[i], up[i - 3]));
        break;
    }
    uint32_t cost = 0;
    for (size_t i = 0; i < stride; i++) cost += out[i] < 128 ? (uint32_t)out[i] : 256u - (uint32_t)out[i];
    return cost;
}

/* ---- zlib-ng, or the stock zlib where only that is packaged */

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
#ifdef MC_ZLIBNG
    return zng_crc32(0, p, (uint32_t)n);
#else
    return (uint32_t)crc32(0, p, (uInt)n);
#endif
}

static size_t deflate_bound(size_t n)
{
#ifdef MC_ZLIBNG
    return zng_compressBound(n);
#else
    return (size_t)compressBound((uLong)n);
#endif
}

/* A zlib stream of src into dst (cap bytes); its length, or 0. */
static size_t deflate_all(uint8_t *dst, size_t cap, const uint8_t *src, size_t n)
{
#ifdef MC_ZLIBNG
    size_t out = cap;
    return zng_compress2(dst, &out, src, n, PNG_LEVEL) == Z_OK ? out : 0;
#else
    uLongf out = (uLongf)cap;
    return compress2(dst, &out, src, (uLong)n, PNG_LEVEL) == Z_OK ? (size_t)out : 0;
#endif
}

/* ---- the file */

static uint8_t *be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return p + 4;
}

/* Writes a chunk whose data is already at p + 8; returns its end. */
static uint8_t *chunk(uint8_t *p, const char *type, size_t len)
{
    be32(p, (uint32_t)len);
    memcpy(p + 4, type, 4);
    return be32(p + 8 + len, crc32_of(p + 4, len + 4));
}

size_t png_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t **out)
{
    *out = NULL;
    size_t stride = (size_t)w * 3;
    /* Keeps the IDAT length, compressed or not, under PNG's 2^31 - 1. */
    if (!w || !h || w > (1u << 24) || stride + 1 > ((size_t)1 << 30) / h) return 0;
    size_t raw_n = (stride + 1) * h;
    uint8_t *raw = mem_alloc(raw_n), *tmp = mem_alloc(stride);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *dst = raw + (size_t)y * (stride + 1);
        uint32_t best = UINT32_MAX;
        /* The first row has no row above; there Up, Average and Paeth are
         * no better than None and Sub, and would read before the image. */
        for (int f = 0; f < (y ? 5 : 2); f++) {
            uint32_t cost = filter_row(rgb, stride, y, f, tmp);
            if (cost < best) {
                best = cost;
                dst[0] = (uint8_t)f;
                memcpy(dst + 1, tmp, stride);
            }
        }
    }
    mem_free(tmp);

    /* Signature, IHDR, IDAT (a zlib stream), IEND. */
    size_t bound = deflate_bound(raw_n);
    uint8_t *png = mem_alloc(8 + 25 + 12 + bound + 12), *p = png;
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    memcpy(p, SIG, 8);
    p += 8;
    uint8_t *d = be32(be32(p + 8, w), h);
    const uint8_t ihdr_tail[5] = {8, 2, 0, 0, 0}; /* 8-bit RGB, deflate, adaptive filters, no interlace */
    memcpy(d, ihdr_tail, 5);
    p = chunk(p, "IHDR", 13);
    size_t zn = deflate_all(p + 8, bound, raw, raw_n);
    mem_free(raw);
    if (!zn) {
        mem_free(png);
        return 0;
    }
    p = chunk(p, "IDAT", zn);
    p = chunk(p, "IEND", 0);
    *out = png;
    return (size_t)(p - png);
}
