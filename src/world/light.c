#include "light.h"

#include "block.h"
#include "jobs.h"
#include "mem.h"

#include <math.h>
#include <string.h>

#define R LIGHT_REGION
#define RGB_MASK 0xFFF0u
#define SKY_MASK 0x000Fu

/* Block light by block, in 4-bit levels per channel. The flame and lava
 * colours follow light_glow at their temperatures; a torch burns a little
 * hotter and whiter than an open campfire, and the oil lamp's wick behind
 * glass is steadier and yellower still. Foxfire is the faint green of
 * the fungus glowing in rotting wood (Panellus, Armillaria). */
uint16_t light_emission(uint8_t id)
{
    switch (id) {
    case B_CAMPFIRE: return LIGHT_PACK(0, 15, 9, 3);
    case B_LAVA: return light_glow(1100.0f);
    case B_TORCH: return LIGHT_PACK(0, 14, 10, 5);
    case B_LAMP: return LIGHT_PACK(0, 15, 13, 8);
    case B_FOXFIRE: return LIGHT_PACK(0, 2, 7, 4);
    default: return 0;
    }
}

/* Black-body colour through the eye: a body starts to glow a dull red at
 * the Draper point (~525 C), is orange at 900 C, yellow at 1100 C and
 * white towards 1500 C. Red rises first and fastest, blue last. */
uint16_t light_glow(float temp_c)
{
    if (temp_c < 500.0f) return 0;
    float r = (temp_c - 500.0f) / 400.0f, g = (temp_c - 800.0f) / 700.0f, b = (temp_c - 1000.0f) / 1000.0f;
    float ch[3] = {r, g, b};
    unsigned v[3] = {0};
    for (int i = 0; i < 3; i++) {
        float c = ch[i] < 0.0f ? 0.0f : (ch[i] > 1.0f ? 1.0f : ch[i]);
        v[i] = (unsigned)lroundf(c * 15.0f);
    }
    if (!v[0]) v[0] = 1; /* anything past the Draper point shows */
    return LIGHT_PACK(0, v[0], v[1], v[2]);
}

int light_passes(uint8_t id) { return !block_opaque(id); }

/* Each of the four levels (sky, R, G, B) the lower of a's and b's. */
static inline uint16_t level_min(uint16_t a, uint16_t b)
{
    uint16_t out = 0;
    for (unsigned s = 0; s < 16; s += 4) {
        unsigned x = (a >> s) & 15u, y = (b >> s) & 15u;
        out = (uint16_t)(out | (x < y ? x : y) << s);
    }
    return out;
}

/* Per channel max of two packed block lights. */
static inline uint16_t rgb_max(uint16_t a, uint16_t b)
{
    uint16_t out = 0;
    for (unsigned s = 4; s < 16; s += 4) {
        unsigned x = (a >> s) & 15u, y = (b >> s) & 15u;
        out = (uint16_t)(out | (x > y ? x : y) << s);
    }
    return out;
}

/* Every block channel one level dimmer, stopping at 0: a bit at the foot
 * of each nonzero nibble, subtracted. */
static inline uint16_t rgb_dim(uint16_t a)
{
    unsigned x = a & RGB_MASK;
    unsigned nz = (x | x >> 1 | x >> 2 | x >> 3) & 0x1110u;
    return (uint16_t)(x - nz);
}

/* The snapshot cells next to cell i (up to six) into nb; returns how many. */
static inline int neighbours(uint32_t i, int top, uint32_t nb[6])
{
    const uint32_t plane = (uint32_t)(R * R);
    uint32_t x = i % (uint32_t)R, z = i / (uint32_t)R % (uint32_t)R, y = i / plane;
    int n = 0;
    if (x > 0) nb[n++] = i - 1;
    if (x < (uint32_t)R - 1) nb[n++] = i + 1;
    if (z > 0) nb[n++] = i - (uint32_t)R;
    if (z < (uint32_t)R - 1) nb[n++] = i + (uint32_t)R;
    if (y > 0) nb[n++] = i - plane;
    if ((int)y < top - 1) nb[n++] = i + plane;
    return n;
}

void light_region(const uint8_t *blocks, int top, const light_emitter *extra, int extra_count, uint16_t *foot)
{
    if (top < 0) top = 0;
    if (top > WORLD_H) top = WORLD_H;
    size_t vol = (size_t)R * R * (size_t)top;
    /* Scratch: the light of every snapshot cell, a FIFO of cells to
     * spread from (each cell at most once at a time, so vol entries
     * suffice) and a flag for "already queued". Uninitialised except
     * where written below: the light array is cleared, the queue only
     * read where written. */
    const int plane = R * R;
    size_t lbytes = (vol * sizeof(uint16_t) + 15) & ~(size_t)15;
    size_t bytes = lbytes + vol * sizeof(uint32_t) + vol + sizeof(int16_t) * (size_t)plane;
    uint8_t *mem = jobs_scratch(bytes);
    uint16_t *lt = (uint16_t *)mem;
    uint32_t *q = (uint32_t *)(mem + lbytes);
    uint8_t *inq = (uint8_t *)(q + vol);
    int16_t *hm = (int16_t *)(inq + vol); /* per x, z: the block that stops the sky, or -1 */
    memset(lt, 0, vol * sizeof *lt);
    memset(inq, 0, vol);

    if (vol) {
        size_t head = 0, tail = 0;
        /* Sky straight down until the first block that stops it. */
        for (int z = 0; z < R; z++)
            for (int x = 0; x < R; x++) {
                int y = top - 1;
                for (; y >= 0; y--) {
                    size_t i = (size_t)y * (size_t)plane + (size_t)(z * R + x);
                    if (!light_passes(blocks[i])) break;
                    lt[i] = 15;
                }
                hm[z * R + x] = (int16_t)y;
            }
        /* Sideways from the open sky into what an overhang or a cliff
         * shades: sky cells beside a column that is stopped higher up. */
        for (int z = 0; z < R; z++)
            for (int x = 0; x < R; x++) {
                int hc = hm[z * R + x], hn = hc;
                if (x > 0 && hm[z * R + x - 1] > hn) hn = hm[z * R + x - 1];
                if (x < R - 1 && hm[z * R + x + 1] > hn) hn = hm[z * R + x + 1];
                if (z > 0 && hm[(z - 1) * R + x] > hn) hn = hm[(z - 1) * R + x];
                if (z < R - 1 && hm[(z + 1) * R + x] > hn) hn = hm[(z + 1) * R + x];
                for (int y = hc + 1; y < hn; y++) {
                    uint32_t i = (uint32_t)((size_t)y * (size_t)plane + (size_t)(z * R + x));
                    q[tail++] = i;
                    inq[i] = 1;
                }
            }
        /* Every seed is 15 and the queue is first in, first out, so cells
         * come off it brightest first and each is lit once. */
        while (head != tail) {
            uint32_t i = q[head++];
            inq[i] = 0;
            unsigned s = lt[i] & SKY_MASK;
            if (s <= 1) continue;
            unsigned ns = s - 1;
            uint32_t nb[6];
            int n = neighbours(i, top, nb);
            for (int k = 0; k < n; k++) {
                uint32_t j = nb[k];
                if ((lt[j] & SKY_MASK) >= ns || !light_passes(blocks[j])) continue;
                lt[j] = (uint16_t)((lt[j] & RGB_MASK) | ns);
                q[tail++] = j;
                inq[j] = 1;
            }
        }

        /* Block light from every emitter. Sources differ in brightness and
         * colour, so a cell can brighten more than once; it goes back on
         * the queue each time unless it is already waiting there. The
         * queue is a ring: at most vol cells wait at once. */
        head = tail = 0;
        size_t count = 0;
        for (size_t i = 0; i < vol; i++) {
            uint16_t e = light_emission(blocks[i]);
            if (!e) continue;
            lt[i] = (uint16_t)(lt[i] | e);
            q[tail] = (uint32_t)i;
            tail = tail + 1 == vol ? 0 : tail + 1;
            inq[i] = 1;
            count++;
        }
        for (int k = 0; k < extra_count; k++) {
            const light_emitter *e = &extra[k];
            if (e->x < 0 || e->x >= R || e->z < 0 || e->z >= R || e->y < 0 || e->y >= top) continue;
            size_t i = ((size_t)e->y * R + (size_t)e->z) * R + (size_t)e->x;
            uint16_t m = (uint16_t)((lt[i] & SKY_MASK) | rgb_max(lt[i], e->light));
            if (m == lt[i]) continue;
            lt[i] = m;
            if (inq[i]) continue;
            q[tail] = (uint32_t)i;
            tail = tail + 1 == vol ? 0 : tail + 1;
            inq[i] = 1;
            count++;
        }
        while (count) {
            uint32_t i = q[head];
            head = head + 1 == vol ? 0 : head + 1;
            count--;
            inq[i] = 0;
            uint16_t d = rgb_dim(lt[i]);
            if (!d) continue;
            uint32_t nb[6];
            int n = neighbours(i, top, nb);
            for (int k = 0; k < n; k++) {
                uint32_t j = nb[k];
                if (!light_passes(blocks[j])) continue;
                uint16_t m = (uint16_t)((lt[j] & SKY_MASK) | rgb_max(lt[j], d));
                if (m == lt[j]) continue;
                lt[j] = m;
                if (inq[j]) continue;
                q[tail] = j;
                tail = tail + 1 == vol ? 0 : tail + 1;
                inq[j] = 1;
                count++;
            }
        }
    }

    /* The middle column and its one-block border. */
    for (int y = 0; y < WORLD_H; y++)
        for (int z = -1; z <= CHUNK_W; z++) {
            uint16_t *dst = &foot[(size_t)y * LIGHT_FOOT * LIGHT_FOOT + (size_t)((z + 1) * LIGHT_FOOT)];
            if (y >= top) {
                for (int x = 0; x < LIGHT_FOOT; x++) dst[x] = LIGHT_OPEN_SKY;
                continue;
            }
            memcpy(dst, &lt[((size_t)y * R + (size_t)(CHUNK_W + z)) * R + CHUNK_W - 1], LIGHT_FOOT * sizeof *dst);
        }
}

uint64_t light_store(const uint16_t *foot, column_light *out)
{
    uint64_t changed = 0;
    for (int sy = 0; sy < SECTIONS; sy++) {
        const uint16_t *s = foot + (size_t)sy * LIGHT_SEC_VOL;
        int uniform = 1;
        for (int i = 1; i < LIGHT_SEC_VOL && uniform; i++) uniform = s[i] == s[0];
        if (uniform) {
            if (out->cells[sy] || out->fill[sy] != s[0]) changed |= 1ull << sy;
            mem_free(out->cells[sy]);
            out->cells[sy] = NULL;
            out->fill[sy] = s[0];
            continue;
        }
        if (out->cells[sy] && !memcmp(out->cells[sy], s, sizeof(uint16_t) * LIGHT_SEC_VOL)) continue;
        if (!out->cells[sy]) out->cells[sy] = mem_alloc(sizeof(uint16_t) * LIGHT_SEC_VOL);
        memcpy(out->cells[sy], s, sizeof(uint16_t) * LIGHT_SEC_VOL);
        changed |= 1ull << sy;
    }
    return changed;
}

void light_free(column_light *l)
{
    for (int sy = 0; sy < SECTIONS; sy++) {
        mem_free(l->cells[sy]);
        l->cells[sy] = NULL;
        l->fill[sy] = 0;
    }
}

uint16_t light_get(const column_light *l, int x, int y, int z)
{
    if (y < 0) return 0;
    if (y >= WORLD_H) return LIGHT_OPEN_SKY;
    int sy = y / SECTION_H;
    return l->cells[sy] ? l->cells[sy][light_fidx(x, y & (SECTION_H - 1), z)] : l->fill[sy];
}

void light_copy_texels(const column_light *l, int sy, uint16_t *out)
{
    for (int y = -1; y <= SECTION_H; y++) {
        int wy = sy * SECTION_H + y;
        for (int z = -1; z <= CHUNK_W; z++) {
            uint16_t *dst = &out[((y + 1) * LIGHT_TEX + (z + 1)) * LIGHT_TEX];
            if (wy >= 0 && wy < WORLD_H && l->cells[wy / SECTION_H]) {
                memcpy(dst, &l->cells[wy / SECTION_H][light_fidx(-1, wy & (SECTION_H - 1), z)],
                       LIGHT_TEX * sizeof *dst);
            } else {
                uint16_t v = light_get(l, 0, wy, z);
                for (int x = 0; x < LIGHT_TEX; x++) dst[x] = v;
            }
        }
    }
}

int light_fix_texels(const uint8_t *blocks, uint16_t *out, uint16_t *uniform)
{
    /* Opaque cells take the dimmest of their open neighbours, level by
     * level. A face samples half a cell into its air, so an opaque texel is
     * only ever blended in along the face, at its edges: the brightest
     * neighbour would bring the far side of a one-block wall (open sky, a
     * torch) into the inside corners of a dark room. Only open cells are
     * read, and only opaque ones written, so the order of the sweep
     * doesn't matter. */
    const int side = LIGHT_TEX, area = LIGHT_TEX * LIGHT_TEX;
    for (int i = 0; i < LIGHT_TEX_VOL; i++) {
        if (light_passes(blocks[i])) continue;
        int x = i % side, z = i / side % side, y = i / area;
        uint16_t v = 0xFFFFu;
        int open = 0;
        const int nb[6] = {x > 0 ? i - 1 : -1,           x < side - 1 ? i + 1 : -1, z > 0 ? i - side : -1,
                           z < side - 1 ? i + side : -1, y > 0 ? i - area : -1,     y < side - 1 ? i + area : -1};
        for (int k = 0; k < 6; k++) {
            int j = nb[k];
            if (j < 0 || !light_passes(blocks[j])) continue;
            v = level_min(v, out[j]);
            open = 1;
        }
        if (open) out[i] = v;
    }
    for (int i = 1; i < LIGHT_TEX_VOL; i++)
        if (out[i] != out[0]) return 0;
    *uniform = out[0];
    return 1;
}

float light_brightness(uint16_t l, float daylight)
{
    unsigned rgb = LIGHT_R(l);
    if (LIGHT_G(l) > rgb) rgb = LIGHT_G(l);
    if (LIGHT_B(l) > rgb) rgb = LIGHT_B(l);
    float sky = powf(0.8f, (float)(LIGHT_MAX - (int)LIGHT_SKY(l))) * daylight;
    float blk = rgb ? powf(0.8f, (float)(LIGHT_MAX - (int)rgb)) : 0.0f;
    return sky > blk ? sky : blk;
}

float light_at(const world *w, double x, double y, double z, float daylight)
{
    int ix = (int)floor(x), iy = (int)floor(y), iz = (int)floor(z);
    return light_brightness(world_light(w, ix, iy, iz), daylight);
}

int light_atlas_layout(uint32_t max_dim, int want, int n[3])
{
    uint32_t xy = max_dim < LIGHT_ATLAS_XY_MAX ? max_dim : LIGHT_ATLAS_XY_MAX;
    uint32_t zd = max_dim < LIGHT_ATLAS_Z_MAX ? max_dim : LIGHT_ATLAS_Z_MAX;
    int cxy = (int)(xy / LIGHT_TEX), cz = (int)(zd / LIGHT_TEX);
    if (cxy < 1) cxy = 1;
    if (cz < 1) cz = 1;
    if (want < 1) want = 1;
    /* A side of 7 slots is 126 texels, just under a power of two: drivers
     * tile 3D images in power-of-two blocks, and Intel's padding of a
     * 15 x 15 x 13 atlas doubled its memory where 14 x 14 x 15 took 4%;
     * 21 a side took 32%, so the side doubles. */
    int top = cxy >= 7 ? cxy - cxy % 7 : cxy, a = top < 7 ? top : 7;
    while (a * 2 <= top && a * a * a < want) a *= 2;
    int z = (want + a * a - 1) / (a * a);
    n[0] = n[1] = a;
    n[2] = z < 1 ? 1 : (z > cz ? cz : z);
    return n[0] * n[1] * n[2];
}

uint32_t light_slot_base(int slot, const int n[3])
{
    uint32_t x = (uint32_t)(slot % n[0]), y = (uint32_t)(slot / n[0] % n[1]), z = (uint32_t)(slot / (n[0] * n[1]));
    return x * LIGHT_TEX | (y * LIGHT_TEX) << 11 | (z * LIGHT_TEX) << 22;
}
