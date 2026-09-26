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
        else if (i >= B_COUNT) c = C_OPAQUE; /* unknown/unloaded: hide faces */
        else if (i == B_WATER) c = C_WATER;
        else if (g_blocks[i].flags & BF_OPAQUE) c = C_OPAQUE;
        else c = C_TRANS;
        g_class[i] = c;
    }
}

static const int DIR[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

static inline int pidx3(const int p[3]) { return mesh_pidx(p[0], p[1], p[2]); }

static inline uint32_t pack_vertex(const int p[3], int face, int ao, int tex, int drop)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 5 | (uint32_t)p[2] << 10 | (uint32_t)face << 15 |
           (uint32_t)ao << 18 | (uint32_t)tex << 20 | (uint32_t)drop << 28;
}

/* Vertex AO from the three blocks around a corner in the layer in front of
 * the face. */
static inline int corner_ao(const mesh_input *in, const int l[3], int u, int v, int su, int sv)
{
    int a[3] = {l[0], l[1], l[2]}, b[3] = {l[0], l[1], l[2]}, c[3] = {l[0], l[1], l[2]};
    a[u] += su;
    b[v] += sv;
    c[u] += su;
    c[v] += sv;
    int s1 = g_class[in->blocks[pidx3(a)]] == C_OPAQUE;
    int s2 = g_class[in->blocks[pidx3(b)]] == C_OPAQUE;
    int cr = g_class[in->blocks[pidx3(c)]] == C_OPAQUE;
    return (s1 && s2) ? 0 : 3 - (s1 + s2 + cr);
}

static uint32_t face_key(const mesh_input *in, const int p[3], int f, int u, int v)
{
    uint8_t b = in->blocks[pidx3(p)];
    uint8_t cb = g_class[b];
    if (cb == C_AIR) return 0;
    int q[3] = {p[0] + DIR[f][0], p[1] + DIR[f][1], p[2] + DIR[f][2]};
    uint8_t n = in->blocks[pidx3(q)];
    uint8_t cn = g_class[n];
    uint32_t key = K_PRESENT | g_blocks[b].tex[f];

    if (cb == C_WATER) {
        int level = in->meta[pidx3(p)] ? in->meta[pidx3(p)] : WATER_FULL;
        int above_water = g_class[in->blocks[mesh_pidx(p[0], p[1] + 1, p[2])]] == C_WATER;
        if (cn == C_WATER || (cn == C_OPAQUE && !(f == 2 && level < WATER_FULL))) return 0;
        int drop = above_water ? 0 : (WATER_FULL - level > 0 ? WATER_FULL - level : 1);
        key |= K_TRANS | (uint32_t)drop << 16 | (uint32_t)3 << 8 | (uint32_t)3 << 10 |
               (uint32_t)3 << 12 | (uint32_t)3 << 14;
        if (f != 2 && f != 3 && drop) key |= K_NOMERGE;
        return key;
    }
    if (cn == C_OPAQUE) return 0;
    if (cb == C_TRANS) {
        if (n == b) return 0;
        key |= K_TRANS;
    }

    /* Corners in (u, v) order: (0,0) (1,0) (1,1) (0,1). */
    int ao0 = corner_ao(in, q, u, v, -1, -1);
    int ao1 = corner_ao(in, q, u, v, +1, -1);
    int ao2 = corner_ao(in, q, u, v, +1, +1);
    int ao3 = corner_ao(in, q, u, v, -1, +1);
    key |= (uint32_t)(ao0 | ao1 << 2 | ao2 << 4 | ao3 << 6) << 8;
    return key;
}

uint32_t mesh_section(const mesh_input *in, uint32_t *out, uint32_t *opaque_quads,
                      uint32_t *trans_quads)
{
    uint32_t nopq = 0, ntr = 0;
    uint32_t mask[16][16];

    for (int f = 0; f < 6; f++) {
        int d = f / 2, sgn = (f & 1) ? -1 : 1;
        int u = (d + 1) % 3, v = (d + 2) % 3;
        for (int slice = 0; slice < 16; slice++) {
            int any = 0;
            for (int j = 0; j < 16; j++)
                for (int i = 0; i < 16; i++) {
                    int p[3];
                    p[d] = slice; p[u] = i; p[v] = j;
                    uint32_t k = face_key(in, p, f, u, v);
                    mask[j][i] = k;
                    any |= k != 0;
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

                    int ao[4] = {(int)(K_AO(k) & 3), (int)(K_AO(k) >> 2 & 3),
                                 (int)(K_AO(k) >> 4 & 3), (int)(K_AO(k) >> 6 & 3)};
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
    }

    /* Translucent quads were written backwards from the end; move them to
     * directly follow the opaque ones. */
    if (ntr)
        memmove(out + 4u * nopq, out + 4u * (MESH_MAX_QUADS - ntr), sizeof(uint32_t) * 4u * ntr);
    *opaque_quads = nopq;
    *trans_quads = ntr;
    return nopq + ntr;
}
