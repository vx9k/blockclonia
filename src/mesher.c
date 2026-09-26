#include "mesher.h"
#include "block.h"
#include "world.h"

#include <string.h>

/* Face key layout. A zero key means "no face". Faces merge only when their
 * keys are identical, so everything that changes a vertex goes in the key. */
#define K_PRESENT   (1u << 31)
#define K_NOMERGE   (1u << 30)
#define K_TRANS     (1u << 29)
#define K_TEX(k)    ((k) & 0xFFu)
#define K_AO(k)     (((k) >> 8) & 0xFFu)
#define K_DROP(k)   (((k) >> 16) & 0xFu)

enum { C_OPAQUE = 1, C_TRANS = 2, C_WATER = 4, C_AIR = 8 };

static uint8_t g_class[256];

void mesher_init(void)
{
    for (int i = 0; i < 256; i++) {
        uint8_t c;
        if (i == B_AIR) c = C_AIR;
        else if (i == B_WATER) c = C_WATER;
        else if (i >= B_COUNT || (g_blocks[i].flags & BF_OPAQUE)) c = C_OPAQUE; /* unknown/unloaded: hide faces */
        else c = C_TRANS;
        g_class[i] = c;
    }
}

static inline uint32_t pack_vertex(const int p[3], int face, int ao, int tex, int drop)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 5 | (uint32_t)p[2] << 10 | (uint32_t)face << 15 |
           (uint32_t)ao << 18 | (uint32_t)tex << 20 | (uint32_t)drop << 28;
}

/* Index strides in the padded array for axes x, y, z: every neighbour is
 * the base index plus a constant, so nothing is recomputed per lookup. */
static const int STRIDE[3] = {1, MESH_PAD * MESH_PAD, MESH_PAD};

/* Vertex AO from the three blocks around a corner in the layer in front of
 * the face (n is that layer's cell, ou/ov the signed tangent strides). */
static inline int corner_ao(const uint8_t *cls, int n, int ou, int ov)
{
    int s1 = cls[n + ou] == C_OPAQUE, s2 = cls[n + ov] == C_OPAQUE, cr = cls[n + ou + ov] == C_OPAQUE;
    return (s1 && s2) ? 0 : 3 - (s1 + s2 + cr);
}

static inline uint32_t face_key(const mesh_input *in, const uint8_t *cls, int pi, int ni, int f, int su, int sv)
{
    uint8_t cb = cls[pi];
    if (cb == C_AIR) return 0;
    uint8_t cn = cls[ni];
    if (cn == C_OPAQUE && cb != C_WATER) return 0; /* hidden: the common case, decided first */
    uint8_t b = in->blocks[pi];
    uint32_t key = K_PRESENT | block_get(b)->tex[f]; /* ids >= B_COUNT read as stone */

    if (cb == C_WATER) {
        int level = in->meta[pi] ? in->meta[pi] : WATER_FULL;
        int above_water = cls[pi + STRIDE[1]] == C_WATER;
        if (cn == C_WATER || (cn == C_OPAQUE && !(f == 2 && level < WATER_FULL))) return 0;
        int drop = above_water ? 0 : (WATER_FULL - level > 0 ? WATER_FULL - level : 1);
        key |= K_TRANS | (uint32_t)drop << 16 | (uint32_t)3 << 8 | (uint32_t)3 << 10 |
               (uint32_t)3 << 12 | (uint32_t)3 << 14;
        if (f != 2 && f != 3 && drop) key |= K_NOMERGE;
        return key;
    }
    if (cb == C_TRANS) {
        if (in->blocks[ni] == b) return 0;
        key |= K_TRANS;
    }

    /* Corners in (u, v) order: (0,0) (1,0) (1,1) (0,1). */
    int ao0 = corner_ao(cls, ni, -su, -sv);
    int ao1 = corner_ao(cls, ni, +su, -sv);
    int ao2 = corner_ao(cls, ni, +su, +sv);
    int ao3 = corner_ao(cls, ni, -su, +sv);
    key |= (uint32_t)(ao0 | ao1 << 2 | ao2 << 4 | ao3 << 6) << 8;
    return key;
}

uint32_t mesh_section_counts(const mesh_input *in, uint32_t *out, mesh_counts *mc)
{
    uint32_t nopq = 0, ntr = 0;
    uint32_t mask[16][16];
    uint8_t cls[MESH_PAD_VOL];
    for (int i = 0; i < MESH_PAD_VOL; i++) cls[i] = g_class[in->blocks[i]];

    for (int f = 0; f < 6; f++) {
        int d = f / 2, sgn = (f & 1) ? -1 : 1;
        int u = (d + 1) % 3, v = (d + 2) % 3;
        int su = STRIDE[u], sv = STRIDE[v], nof = sgn * STRIDE[d];
        for (int slice = 0; slice < 16; slice++) {
            int any = 0;
            int base = mesh_pidx(0, 0, 0) + slice * STRIDE[d];
            for (int j = 0; j < 16; j++) {
                int pi = base + j * sv;
                for (int i = 0; i < 16; i++, pi += su) {
                    uint32_t k = face_key(in, cls, pi, pi + nof, f, su, sv);
                    mask[j][i] = k;
                    any |= k != 0;
                }
            }
            if (!any) continue;

            for (int j = 0; j < 16; j++)
                for (int i = 0; i < 16;) {
                    uint32_t k = mask[j][i];
                    if (!k) { i++; continue; }
                    int w = 1, h = 1;
                    if (!(k & K_NOMERGE)) {
                        while (i + w < 16 && mask[j][i + w] == k) w++;
                        for (; j + h < 16; h++) {
                            int x = 0;
                            while (x < w && mask[j + h][i + x] == k) x++;
                            if (x < w) break;
                        }
                    }
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++) mask[j + y][i + x] = 0;

                    const int ao[4] = {(int)(K_AO(k) & 3), (int)(K_AO(k) >> 2 & 3), (int)(K_AO(k) >> 4 & 3),
                                       (int)(K_AO(k) >> 6 & 3)};
                    static const int CU[4] = {0, 1, 1, 0}, CV[4] = {0, 0, 1, 1};
                    /* Counter-clockwise seen from outside the face. */
                    int order[4] = {0, 1, 2, 3};
                    if (sgn < 0) { order[1] = 3; order[3] = 1; }
                    /* Flip the split diagonal so AO interpolates without a
                     * visible crease: rotate the quad by one vertex. */
                    int start = (ao[order[0]] + ao[order[2]] < ao[order[1]] + ao[order[3]]) ? 1 : 0;

                    uint32_t *dst;
                    if (k & K_TRANS) {
                        ntr++;
                        dst = out + 4u * (MESH_MAX_QUADS - ntr);
                    } else {
                        dst = out + 4u * nopq;
                        nopq++;
                    }
                    int plane = slice + (sgn > 0);
                    for (int n = 0; n < 4; n++) {
                        int c = order[(n + start) & 3];
                        int p[3];
                        p[d] = plane;
                        p[u] = i + CU[c] * w;
                        p[v] = j + CV[c] * h;
                        /* Only vertices on the top edge of a water block drop. */
                        int drop = (int)K_DROP(k);
                        if (drop && d != 1) {
                            int top_edge = (u == 1) ? CU[c] : CV[c];
                            if (!top_edge) drop = 0;
                        } else if (drop && f == 3) {
                            drop = 0;
                        }
                        dst[n] = pack_vertex(p, f, ao[c], (int)K_TEX(k), drop);
                    }
                    i += w;
                }
        }
        mc->face_end[f] = (uint16_t)nopq;
    }

    /* Translucent quads were written backwards from the end; move them to
     * directly follow the opaque ones. */
    if (ntr)
        memmove(out + 4u * nopq, out + 4u * (MESH_MAX_QUADS - ntr), sizeof(uint32_t) * 4u * ntr);
    mc->opaque = nopq;
    mc->trans = ntr;
    return nopq + ntr;
}

uint32_t mesh_section(const mesh_input *in, uint32_t *out, uint32_t *opaque_quads, uint32_t *trans_quads)
{
    mesh_counts mc;
    uint32_t n = mesh_section_counts(in, out, &mc);
    *opaque_quads = mc.opaque;
    *trans_quads = mc.trans;
    return n;
}
