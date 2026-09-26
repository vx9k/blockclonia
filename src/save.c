#include "save.h"
#include "mem.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* File layout (little endian):
 *   0  "MCCL"        magic
 *   4  u8  version   (1)
 *   5  u8  flags     bit 0: meta present
 *   6  u16 reserved  (0)
 *   8  i32 cx, 12 i32 cz
 *  16  u32 payload length
 *  20  u32 FNV-1a of payload
 *  24  payload: RLE blocks, then RLE meta if flagged.
 *      RLE = pairs of (run length - 1, value). */
#define HEADER_SIZE 24
#define SAVE_VERSION 1

static const uint8_t MAGIC[4] = {'M', 'C', 'C', 'L'};

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619U; }
    return h;
}

static size_t rle_encode(const uint8_t *src, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        uint8_t v = src[i];
        size_t run = 1;
        while (i + run < n && run < 256 && src[i + run] == v) run++;
        if (o + 2 > cap) return 0;
        out[o++] = (uint8_t)(run - 1);
        out[o++] = v;
        i += run;
    }
    return o;
}

/* Decodes exactly n values; returns bytes consumed, or 0 on error. */
static size_t rle_decode(const uint8_t *in, size_t len, uint8_t *dst, size_t n, uint8_t max_value)
{
    size_t i = 0, o = 0;
    while (o < n) {
        if (len - i < 2) return 0;
        size_t run = (size_t)in[i] + 1;
        uint8_t v = in[i + 1];
        i += 2;
        if (v > max_value || run > n - o) return 0;
        memset(dst + o, v, run);
        o += run;
    }
    return i;
}

size_t save_encode_column(const column *c, uint8_t *out, size_t cap)
{
    if (cap < HEADER_SIZE) return 0;
    int has_meta = 0;
    if (c->meta)
        for (size_t i = 0; i < COL_VOL && !has_meta; i++) has_meta = c->meta[i] != 0;

    size_t n = rle_encode(c->blocks, COL_VOL, out + HEADER_SIZE, cap - HEADER_SIZE);
    if (!n) return 0;
    size_t payload = n;
    if (has_meta) {
        n = rle_encode(c->meta, COL_VOL, out + HEADER_SIZE + payload, cap - HEADER_SIZE - payload);
        if (!n) return 0;
        payload += n;
    }
    memcpy(out, MAGIC, 4);
    out[4] = SAVE_VERSION;
    out[5] = (uint8_t)has_meta;
    out[6] = out[7] = 0;
    put_u32(out + 8, (uint32_t)c->cx);
    put_u32(out + 12, (uint32_t)c->cz);
    put_u32(out + 16, (uint32_t)payload);
    put_u32(out + 20, fnv1a(out + HEADER_SIZE, payload));
    return HEADER_SIZE + payload;
}

int save_decode_column(column *c, const uint8_t *data, size_t len)
{
    if (len < HEADER_SIZE || len > SAVE_MAX_FILE) return -1;
    if (memcmp(data, MAGIC, 4) != 0 || data[4] != SAVE_VERSION) return -1;
    uint8_t flags = data[5];
    if ((flags & ~1u) || data[6] || data[7]) return -1;
    if ((int32_t)get_u32(data + 8) != c->cx || (int32_t)get_u32(data + 12) != c->cz) return -1;
    uint32_t payload = get_u32(data + 16);
    if (payload != len - HEADER_SIZE) return -1;
    const uint8_t *p = data + HEADER_SIZE;
    if (fnv1a(p, payload) != get_u32(data + 20)) return -1;

    size_t used = rle_decode(p, payload, c->blocks, COL_VOL, B_COUNT - 1);
    if (!used) return -1;
    if (flags & 1) {
        if (!c->meta) c->meta = mem_alloc(COL_VOL);
        size_t m = rle_decode(p + used, payload - used, c->meta, COL_VOL, WATER_FULL - 1);
        if (!m) return -1;
        used += m;
        for (size_t i = 0; i < COL_VOL; i++)
            if (c->meta[i] && c->blocks[i] != B_WATER) return -1;
    } else {
        mem_free(c->meta);
        c->meta = NULL;
    }
    return used == payload ? 0 : -1;
}

static int column_path(char *buf, size_t cap, const char *dir, int cx, int cz, const char *suffix)
{
    int n = snprintf(buf, cap, "%s/c.%d.%d.bin%s", dir, cx, cz, suffix);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int save_load_column(const char *dir, column *c)
{
    char path[512];
    if (column_path(path, sizeof path, dir, c->cx, c->cz, "") != 0) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *buf = mem_alloc(SAVE_MAX_FILE + 1);
    size_t len = fread(buf, 1, SAVE_MAX_FILE + 1, f);
    int err = ferror(f);
    fclose(f);
    int r = (!err && len <= SAVE_MAX_FILE && save_decode_column(c, buf, len) == 0) ? 1 : -1;
    mem_free(buf);
    return r;
}

int save_store_column(const char *dir, const column *c)
{
    char path[512], tmp[512];
    if (column_path(path, sizeof path, dir, c->cx, c->cz, "") != 0) return -1;
    if (column_path(tmp, sizeof tmp, dir, c->cx, c->cz, ".tmp") != 0) return -1;
    uint8_t *buf = mem_alloc(SAVE_MAX_FILE);
    size_t len = save_encode_column(c, buf, SAVE_MAX_FILE);
    int r = -1;
    if (len) {
        FILE *f = fopen(tmp, "wb");
        if (f) {
            int ok = fwrite(buf, 1, len, f) == len;
            ok &= fflush(f) == 0;
            ok &= fclose(f) == 0;
            /* Write-to-temp then rename: a crash mid-save never leaves a
             * truncated column file behind. */
            if (ok && rename(tmp, path) == 0) r = 0;
            else remove(tmp);
        }
    }
    mem_free(buf);
    return r;
}

int save_ensure_dir(const char *dir)
{
    if (mkdir(dir, 0755) == 0 || errno == EEXIST) {
        struct stat st;
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    return -1;
}

int save_read_seed(const char *dir, uint32_t *seed)
{
    char path[512], buf[64];
    int n = snprintf(path, sizeof path, "%s/level.dat", dir);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = '\0';
    if (strncmp(buf, "seed ", 5) != 0) return -1;
    char *end;
    errno = 0;
    unsigned long v = strtoul(buf + 5, &end, 10);
    if (errno || end == buf + 5 || (*end != '\n' && *end != '\0') || v > UINT32_MAX) return -1;
    *seed = (uint32_t)v;
    return 0;
}

int save_write_seed(const char *dir, uint32_t seed)
{
    char path[512];
    int n = snprintf(path, sizeof path, "%s/level.dat", dir);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fprintf(f, "seed %u\n", seed) > 0;
    ok &= fclose(f) == 0;
    return ok ? 0 : -1;
}
