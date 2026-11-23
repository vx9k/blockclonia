/* The PNG encoder, checked by decoding what it writes: every chunk and its
 * CRC, the header, the zlib stream inflated by zlib-ng and the PNG filters
 * undone, back to the pixels that went in. */
#include "../src/core/mem.h"
#include "../src/core/png.h"
#include "test_util.h"

#include <string.h>
#ifdef MC_ZLIBNG
#include <zlib-ng.h>
static uint32_t crc_of(const uint8_t *p, uint32_t n) { return zng_crc32(0, p, n); }
static int inflate_to(uint8_t *dst, size_t *dn, const uint8_t *src, size_t n)
{
    return zng_uncompress(dst, dn, src, n) == Z_OK;
}
#else
#include <zlib.h>
static uint32_t crc_of(const uint8_t *p, uint32_t n) { return (uint32_t)crc32(0, p, n); }
static int inflate_to(uint8_t *dst, size_t *dn, const uint8_t *src, size_t n)
{
    uLongf out = (uLongf)*dn;
    int ok = uncompress(dst, &out, src, (uLong)n) == Z_OK;
    *dn = (size_t)out;
    return ok;
}
#endif

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
}

/* Undoes the filters of the inflated image data into img; 1 when every
 * row names a known filter (and the first only None or Sub, which need no
 * row above). */
static int unfilter(const uint8_t *raw, uint32_t w, uint32_t h, uint8_t *img)
{
    size_t stride = (size_t)w * 3;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = raw + (size_t)y * (stride + 1);
        uint8_t *row = img + (size_t)y * stride;
        const uint8_t *up = y ? row - stride : NULL;
        int f = src[0];
        if (f > 4 || (!up && f > 1)) return 0;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= 3 ? row[i - 3] : 0, b = up ? up[i] : 0, c = (up && i >= 3) ? up[i - 3] : 0;
            int pred = f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, c) : 0;
            row[i] = (uint8_t)(src[1 + i] + pred);
        }
    }
    return 1;
}

/* Decodes a PNG from png_encode back to RGB; 1 when it matches rgb. With
 * filters, also gives each row's filter type (h bytes). */
static int decodes_to(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *filters)
{
    uint8_t *png = NULL;
    size_t n = png_encode(rgb, w, h, &png);
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    int ok = n > 8 + 25 + 12 + 12 && memcmp(png, SIG, 8) == 0;
    size_t stride = (size_t)w * 3, raw_n = (stride + 1) * h;
    uint8_t *raw = mem_alloc(raw_n + 1), *img = mem_alloc(stride * h + 1);
    int chunks = 0, saw_end = 0;
    size_t pos = 8;
    while (ok && pos + 12 <= n && !saw_end) {
        uint32_t len = rd32(png + pos);
        const uint8_t *type = png + pos + 4, *data = png + pos + 8;
        ok = pos + 12 + len <= n && crc_of(type, len + 4) == rd32(data + len);
        if (ok && chunks == 0) {
            ok = memcmp(type, "IHDR", 4) == 0 && len == 13 && rd32(data) == w && rd32(data + 4) == h && data[8] == 8 &&
                 data[9] == 2 && data[10] == 0 && data[11] == 0 && data[12] == 0;
        } else if (ok && chunks == 1) {
            size_t got = raw_n + 1;
            ok = memcmp(type, "IDAT", 4) == 0 && inflate_to(raw, &got, data, len) && got == raw_n;
        } else if (ok) {
            saw_end = memcmp(type, "IEND", 4) == 0 && len == 0;
        }
        pos += 12 + len;
        chunks++;
    }
    ok = ok && saw_end && pos == n && chunks == 3 && unfilter(raw, w, h, img) && memcmp(img, rgb, stride * h) == 0;
    for (uint32_t y = 0; ok && filters && y < h; y++) filters[y] = raw[(size_t)y * (stride + 1)];
    mem_free(raw);
    mem_free(img);
    mem_free(png);
    return ok;
}

static int round_trips(const uint8_t *rgb, uint32_t w, uint32_t h) { return decodes_to(rgb, w, h, NULL); }

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void test_png_images(void)
{
    const uint8_t px[3] = {200, 10, 99};
    CHECK(round_trips(px, 1, 1));

    enum { W = 333, H = 97 };
    uint8_t *img = mem_alloc((size_t)W * H * 3);
    /* Noise: nothing to compress, the stream's worst case. */
    uint32_t s = 7;
    for (size_t i = 0; i < (size_t)W * H * 3; i++) img[i] = (uint8_t)lcg(&s);
    CHECK(round_trips(img, W, H));
    /* Gradients and flat bands, like sky and terrain, so every filter
     * wins somewhere. */
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *p = img + ((size_t)y * W + x) * 3;
            p[0] = (uint8_t)(x + y);
            p[1] = y < H / 2 ? 120 : (uint8_t)(x * 7 ^ y);
            p[2] = (uint8_t)(255 - y * 2);
        }
    CHECK(round_trips(img, W, H));
    /* A frame of one colour compresses to almost nothing. */
    memset(img, 77, (size_t)W * H * 3);
    uint8_t *png = NULL;
    size_t n = png_encode(img, W, H, &png);
    CHECK(n > 0 && n < (size_t)W * H * 3 / 100);
    mem_free(png);
    CHECK(round_trips(img, W, H));

    /* The filter choice: noisy rows that each repeat the row above leave
     * nothing but zeros under Up, and a steady left-to-right ramp nothing
     * but its step under Sub; the first row may only use None or Sub. */
    uint8_t filters[H] = {0};
    s = 3;
    for (size_t i = 0; i < (size_t)W * 3; i++) img[i] = (uint8_t)lcg(&s);
    for (uint32_t y = 1; y < H; y++) memcpy(img + (size_t)y * W * 3, img, (size_t)W * 3);
    CHECK(decodes_to(img, W, H, filters));
    CHECK(filters[0] <= 1 && filters[1] == 2 && filters[H - 1] == 2);
    for (uint32_t y = 0; y < H; y++)
        for (size_t i = 0; i < (size_t)W * 3; i++) img[(size_t)y * W * 3 + i] = (uint8_t)(i * 5 + y * 77);
    CHECK(decodes_to(img, W, H, filters));
    CHECK(filters[0] == 1 && filters[H / 2] == 1);
    mem_free(img);

    /* Nothing to encode, or more than one IDAT chunk could hold. */
    uint8_t sentinel = 0;
    png = &sentinel; /* must come back NULL */
    CHECK(png_encode(px, 0, 1, &png) == 0 && png == NULL);
    CHECK(png_encode(px, 1, 0, &png) == 0 && png == NULL);
    CHECK(png_encode(px, 1u << 25, 1, &png) == 0 && png == NULL);
    CHECK(png_encode(px, 100000, 100000, &png) == 0 && png == NULL);
}

void test_png_all(void) { test_png_images(); }
