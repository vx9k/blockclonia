#include "save.h"
#include "mem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
            if (c->meta[i] && !block_has_meta(c->blocks[i])) return -1;
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

/* World folders can come from anyone, so file access never follows a
 * symlink and only touches regular files. Otherwise a shared world could
 * point level.dat or a column file at, say, ~/.bashrc and have the game
 * overwrite it on save, or at a FIFO and hang a loader thread. */

/* Reads up to `cap` bytes of a regular file. Returns the length read,
 * cap + 1 if the file is larger, or -1 if it is missing or unusable. */
static long read_regular(const char *path, uint8_t *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    if ((unsigned long long)st.st_size > cap) {
        close(fd);
        return (long)cap + 1;
    }
    size_t len = 0;
    while (len < cap) {
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    return (long)len;
}

/* Writes a whole file through a fresh temp file and a rename, so a crash
 * mid-save never leaves a truncated file, and whatever sat at either name
 * before (a symlink, a hard link to another file) is replaced, never
 * written through. */
static int write_atomic(const char *path, const char *tmp, const void *data, size_t len)
{
    unlink(tmp); /* a stale or planted temp entry; unlink never follows */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    int ok = done == len;
    ok &= close(fd) == 0;
    if (ok && rename(tmp, path) == 0) return 0;
    unlink(tmp);
    return -1;
}

int save_load_column(const char *dir, column *c)
{
    char path[512];
    if (column_path(path, sizeof path, dir, c->cx, c->cz, "") != 0) return 0;
    uint8_t *buf = mem_alloc(SAVE_MAX_FILE);
    long len = read_regular(path, buf, SAVE_MAX_FILE);
    int r = 0; /* missing: generate */
    if (len >= 0 && len <= SAVE_MAX_FILE) r = save_decode_column(c, buf, (size_t)len) == 0 ? 1 : -1;
    else if (len >= 0 || errno != ENOENT) r = -1; /* too big, or not a readable regular file */
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
    int r = len ? write_atomic(path, tmp, buf, len) : -1;
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
    long len = read_regular(path, (uint8_t *)buf, sizeof buf - 1);
    if (len < 0 || len > (long)sizeof buf - 1) return -1;
    buf[len] = '\0';
    /* strtoul would accept spaces, '+' and '-' (negation wraps): digits only. */
    if (strncmp(buf, "seed ", 5) != 0 || buf[5] < '0' || buf[5] > '9') return -1;
    char *end;
    errno = 0;
    unsigned long v = strtoul(buf + 5, &end, 10);
    if (errno || end == buf + 5 || (*end != '\n' && *end != '\0') || v > UINT32_MAX) return -1;
    *seed = (uint32_t)v;
    return 0;
}

int save_write_seed(const char *dir, uint32_t seed)
{
    char path[512], tmp[512], text[32];
    int n = snprintf(path, sizeof path, "%s/level.dat", dir);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    n = snprintf(tmp, sizeof tmp, "%s/level.dat.tmp", dir);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    int len = snprintf(text, sizeof text, "seed %u\n", seed);
    return write_atomic(path, tmp, text, (size_t)len);
}

long save_read_file(const char *path, void *buf, size_t cap)
{
    long len = read_regular(path, buf, cap);
    return len >= 0 && (size_t)len <= cap ? len : -1;
}

int save_write_file(const char *path, const void *data, size_t len)
{
    char tmp[512];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    return write_atomic(path, tmp, data, len);
}
