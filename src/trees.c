#include "trees.h"
#include "mem.h"
#include "noise.h"
#include "physics.h"

#include <stdlib.h>
#include <string.h>

/* Shapes are integer arithmetic throughout, so a tree grows the same on
 * every machine and compiler. */

typedef struct {
    int x, y, z;
} pos;

typedef struct {
    tree_shape *t;
    uint32_t s;
    tree_age age;
    uint8_t log, leaf;
} grower;

static uint32_t rnd(grower *g)
{
    g->s = hash_u32(g->s + 0x9E3779B9u);
    return g->s;
}

/* lo..hi, both included. */
static int rand_in(grower *g, int lo, int hi) { return lo + (int)(rnd(g) % (uint32_t)(hi - lo + 1)); }

static int chance(grower *g, int percent) { return (int)(rnd(g) % 100u) < percent; }

static int inside(int x, int y, int z)
{
    return x >= -TREE_REACH && x <= TREE_REACH && z >= -TREE_REACH && z <= TREE_REACH && y >= -TREE_DOWN && y < TREE_UP;
}

static void wood(grower *g, int x, int y, int z, int axis)
{
    if (!inside(x, y, z)) return;
    int i = tree_cell(x, y, z);
    g->t->id[i] = g->log;
    g->t->meta[i] = (uint8_t)axis;
}

static void put(grower *g, int x, int y, int z, uint8_t id)
{
    if (!inside(x, y, z)) return;
    int i = tree_cell(x, y, z);
    if (g->t->id[i] == B_AIR) g->t->id[i] = id;
}

static void trunk(grower *g, int x, int z, int y0, int y1)
{
    for (int y = y0; y < y1; y++) wood(g, x, y, z, LOG_UP);
}

/* The eight ways out from a trunk; limb i of n leaves at an even spread
 * from a random start. */
static const int D8[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};

/* A limb from p: len steps out along (dx, dz), climbing `rise` blocks on
 * the way, every block face to face with the last so it carries load. A
 * step out costs a block of the wood's span (physics.c), a step up none,
 * so a diagonal step, two blocks across, costs two. Returns its end. */
static pos limb(grower *g, pos p, int dx, int dz, int len, int rise)
{
    for (int i = 1; i <= len; i++) {
        if (dx) {
            p.x += dx;
            wood(g, p.x, p.y, p.z, LOG_BRANCH);
        }
        if (dz) {
            p.z += dz;
            wood(g, p.x, p.y, p.z, LOG_BRANCH);
        }
        for (int k = rise * i / len - rise * (i - 1) / len; k > 0; k--) wood(g, p.x, ++p.y, p.z, LOG_BRANCH);
    }
    return p;
}

/* Diagonal limbs go two thirds as many steps, to reach about as far. */
static int reach(int dir, int len) { return dir & 1 ? (len * 2 + 2) / 3 : len; }

/* Leaves in an ellipsoid of radii (rx, ry, rz) round c, its outer shell
 * ragged: `ragged` in a hundred of the cells past about three quarters of
 * the way out are left open. */
static void blob(grower *g, pos c, int rx, int ry, int rz, int ragged)
{
    long ax = (long)ry * rz, ay = (long)rx * rz, az = (long)rx * ry, lim = (long)rx * ry * rz;
    lim *= lim;
    for (int dy = -ry; dy <= ry; dy++)
        for (int dz = -rz; dz <= rz; dz++)
            for (int dx = -rx; dx <= rx; dx++) {
                long d = (long)dx * dx * ax * ax + (long)dy * dy * ay * ay + (long)dz * dz * az * az;
                if (d > lim + lim / 4) continue;
                if (d * 20 > lim * 11 && chance(g, ragged)) continue;
                put(g, c.x + dx, c.y + dy, c.z + dz, g->leaf);
            }
}

/* A flat disc of leaves, radius r, at height y. */
static void disc(grower *g, pos c, int r, int ragged)
{
    for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
            int d = dx * dx + dz * dz;
            if (d > r * r + r) continue;
            if (d >= r * r - r && chance(g, ragged)) continue;
            put(g, c.x + dx, c.y, c.z + dz, g->leaf);
        }
}

/* Old trees flare at the foot into roots under the soil. */
static void roots(grower *g, int x0, int z0, int x1, int z1)
{
    for (int k = 0; k < 4; k++) {
        int dx = D8[k * 2][0], dz = D8[k * 2][1];
        int x = dx > 0 ? x1 + 1 : (dx < 0 ? x0 - 1 : rand_in(g, x0, x1)),
            z = dz > 0 ? z1 + 1 : (dz < 0 ? z0 - 1 : rand_in(g, z0, z1));
        int n = rand_in(g, 1, 3);
        for (int i = 0; i < n; i++) wood(g, x + dx * i, -1 - (i == n - 1 && n > 1), z + dz * i, LOG_BRANCH);
    }
}

static void spread(grower *g, pos from, int n, int len, int rise_lo, int rise_hi, int rx, int ry, int ragged)
{
    int start = rand_in(g, 0, 7);
    for (int i = 0; i < n; i++) {
        int d = (start + i * 8 / n) & 7;
        pos e = limb(g, from, D8[d][0], D8[d][1], reach(d, len), rand_in(g, rise_lo, rise_hi));
        e.y++;
        blob(g, e, rx, ry, rx, ragged);
    }
}

/* Oak: a short, stout trunk that forks into a few big limbs, each under a
 * rounded mass of leaves; wide as it is tall. */
static void oak(grower *g)
{
    int h = rand_in(g, 4, 6) + (g->age == AGE_OLD);
    if (g->age == AGE_YOUNG) {
        h = rand_in(g, 3, 4);
        trunk(g, 0, 0, 0, h);
        blob(g, (pos){0, h, 0}, 2, 2, 2, 30);
        return;
    }
    trunk(g, 0, 0, 0, h + 1);
    int old = g->age == AGE_OLD;
    spread(g, (pos){0, h - 1 - rand_in(g, 0, 1), 0}, old ? rand_in(g, 4, 5) : rand_in(g, 2, 4), 2 + old, 1, 3, 3, 2,
           35);
    blob(g, (pos){0, h + 2, 0}, 2, 2, 2, 30);
    if (old) roots(g, 0, 0, 0, 0);
}

/* Birch: tall and slim, short limbs turned up along the upper trunk, each
 * with a small tuft, and a narrow open crown. */
static void birch(grower *g)
{
    int h = g->age == AGE_YOUNG ? rand_in(g, 4, 6) : rand_in(g, 8, 11) + (g->age == AGE_OLD) * 2;
    trunk(g, 0, 0, 0, h);
    for (int y = h - 6; y < h - 1; y += 2) {
        if (y < 2) continue;
        int d = rand_in(g, 0, 7);
        pos e = limb(g, (pos){0, y, 0}, D8[d][0], D8[d][1], 1, 1);
        blob(g, (pos){e.x, e.y + 1, e.z}, rand_in(g, 1, 2), 1, rand_in(g, 1, 2), 35);
    }
    blob(g, (pos){0, h, 0}, 2, 3, 2, 40);
}

/* Spruce: a cone of tiers from low on the trunk to a spike, each tier a
 * ring of short branches under a skirt of needles. */
static void spruce(grower *g)
{
    int h = g->age == AGE_YOUNG ? rand_in(g, 5, 7) : rand_in(g, 10, 14) + (g->age == AGE_OLD) * 3;
    trunk(g, 0, 0, 0, h);
    for (int y = g->age == AGE_YOUNG ? 1 : 2; y < h; y += 2) {
        int r = 1 + (h - y) * 3 / h;
        if (r >= 2)
            for (int k = 0; k < 4; k++) limb(g, (pos){0, y, 0}, D8[k * 2][0], D8[k * 2][1], r - 1, 0);
        disc(g, (pos){0, y, 0}, r, 30);
        disc(g, (pos){0, y + 1, 0}, r - 1, 20);
    }
    put(g, 0, h, 0, g->leaf);
    put(g, 0, h + 1, 0, g->leaf);
}

/* Scots pine: a long bare trunk, its crown a few flat pads of needles on
 * short limbs at the very top. */
static void pine(grower *g)
{
    int h = g->age == AGE_YOUNG ? rand_in(g, 5, 7) : rand_in(g, 10, 13) + (g->age == AGE_OLD) * 2;
    trunk(g, 0, 0, 0, h);
    spread(g, (pos){0, h - rand_in(g, 2, 3), 0}, rand_in(g, 2, 3), rand_in(g, 1, 2), 1, 1, 2, 1, 35);
    blob(g, (pos){0, h, 0}, 2, 1, 2, 30);
}

/* Thorn tree: short, forking low into a few limbs under a wide, thin,
 * flat crown that shades the ground under it. */
static void thorn(grower *g)
{
    int h = g->age == AGE_YOUNG ? 2 : rand_in(g, 3, 4);
    trunk(g, 0, 0, 0, h);
    int len = g->age == AGE_OLD ? 3 : 2, start = rand_in(g, 0, 7), n = rand_in(g, 2, 3);
    for (int i = 0; i < n; i++) {
        int d = (start + i * 8 / n) & 7;
        pos e = limb(g, (pos){0, h - 1, 0}, D8[d][0], D8[d][1], reach(d, len), 1);
        disc(g, (pos){e.x, e.y + 1, e.z}, 3, 35);
        disc(g, (pos){e.x, e.y + 2, e.z}, 1, 40);
    }
    disc(g, (pos){0, h, 0}, g->age == AGE_YOUNG ? 2 : 3, 35);
}

/* Willow: a short trunk whose arching limbs end in mops of leaves that
 * hang in curtains nearly to the ground; it grows by water. */
static void willow(grower *g)
{
    int h = g->age == AGE_YOUNG ? 3 : rand_in(g, 4, 6);
    trunk(g, 0, 0, 0, h + 1);
    int n = g->age == AGE_YOUNG ? 2 : rand_in(g, 3, 5);
    spread(g, (pos){0, h - 1, 0}, n, g->age == AGE_YOUNG ? 1 : rand_in(g, 2, 3), 1, 2, 2, 1, 30);
    blob(g, (pos){0, h + 1, 0}, 2, 2, 2, 30);
    if (g->age == AGE_OLD) roots(g, 0, 0, 0, 0);
}

/* The curtains: from every leaf at the crown's edge with air under it,
 * leaves hang a few blocks. Willow leaves bend further (their span). */
static void curtains(grower *g)
{
    tree_shape *t = g->t;
    for (int y = TREE_UP - 1; y > 1; y--)
        for (int z = -TREE_REACH; z <= TREE_REACH; z++)
            for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
                if (abs(x) + abs(z) < 2 || t->id[tree_cell(x, y, z)] != g->leaf) continue;
                if (t->id[tree_cell(x, y - 1, z)] != B_AIR || !chance(g, 55)) continue;
                int n = rand_in(g, 2, 4);
                for (int k = 1; k <= n && y - k > 0; k++) {
                    int i = tree_cell(x, y - k, z);
                    if (t->id[i] != B_AIR) break;
                    t->id[i] = g->leaf;
                }
            }
}

/* Maple: limbs climbing steeply to hold up a full dome. */
static void maple(grower *g)
{
    if (g->age == AGE_YOUNG) {
        int h = rand_in(g, 3, 5);
        trunk(g, 0, 0, 0, h);
        blob(g, (pos){0, h + 1, 0}, 2, 2, 2, 25);
        return;
    }
    int h = rand_in(g, 5, 7);
    trunk(g, 0, 0, 0, h + 3);
    spread(g, (pos){0, h - 1, 0}, 4, 2, 2, 3, 2, 2, 25);
    blob(g, (pos){0, h + 2, 0}, 3 + (g->age == AGE_OLD), 3, 3 + (g->age == AGE_OLD), 25);
    if (g->age == AGE_OLD) roots(g, 0, 0, 0, 0);
}

/* Cherry: short, with long limbs spread nearly level, in blossom. */
static void cherry(grower *g)
{
    int h = g->age == AGE_YOUNG ? 2 : rand_in(g, 3, 4);
    trunk(g, 0, 0, 0, h + 1);
    int n = g->age == AGE_YOUNG ? 2 : rand_in(g, 3, 5);
    spread(g, (pos){0, h - 1, 0}, n, g->age == AGE_YOUNG ? 1 : rand_in(g, 2, 3), 1, 1, 2, 1, 25);
    blob(g, (pos){0, h + 1, 0}, 2, 1, 2, 25);
}

/* Redwood: a trunk two blocks across and twenty or thirty tall, flared
 * into buttresses at its foot, bare to over half its height, then short
 * limbs with small sprays of needles up to a narrow spire. */
static void redwood(grower *g)
{
    if (g->age == AGE_YOUNG) {
        int h = rand_in(g, 9, 12);
        trunk(g, 0, 0, 0, h);
        for (int y = h / 2; y < h; y += 2) disc(g, (pos){0, y, 0}, 1 + (y < h - 3), 30);
        put(g, 0, h, 0, g->leaf);
        return;
    }
    int h = rand_in(g, 20, 27) + (g->age == AGE_OLD) * 6;
    for (int k = 0; k < 4; k++) trunk(g, k & 1, k >> 1, 0, h);
    /* Buttresses: the trunk's foot on each side, a block or two up. */
    static const int B[8][2] = {{-1, 0}, {-1, 1}, {2, 0}, {2, 1}, {0, -1}, {1, -1}, {0, 2}, {1, 2}};
    for (int k = 0; k < 8; k++)
        if (chance(g, 55)) trunk(g, B[k][0], B[k][1], 0, rand_in(g, 1, 2 + (g->age == AGE_OLD)));
    roots(g, 0, 0, 1, 1);
    for (int y = h * 55 / 100; y < h; y += 2)
        for (int k = 0; k < 2; k++) {
            int d = rand_in(g, 0, 7), dx = D8[d][0], dz = D8[d][1];
            pos e = limb(g, (pos){dx > 0, y, dz > 0}, dx, dz, y > h - 5 ? 1 : rand_in(g, 1, 2), rand_in(g, 0, 1));
            disc(g, e, y > h - 5 ? 1 : 2, 35);
            disc(g, (pos){e.x, e.y + 1, e.z}, 1, 30);
        }
    for (int k = 0; k < 4; k++) put(g, k & 1, h, k >> 1, g->leaf);
    put(g, 0, h + 1, 0, g->leaf);
    put(g, 1, h + 1, 1, g->leaf);
}

/* Palm: a slim trunk leaning a step or two as it climbs, a tuft of
 * fronds arching out from the top and drooping at their tips. */
static void palm(grower *g)
{
    int h = g->age == AGE_YOUNG ? rand_in(g, 3, 4) : rand_in(g, 6, 9), d = rand_in(g, 0, 3) * 2, x = 0, z = 0;
    int lean = g->age == AGE_YOUNG ? 0 : rand_in(g, 1, 2), stepped = 0;
    for (int y = 0; y < h; y++) {
        if (y >= 2 && y % 3 == 2 && stepped < lean) {
            /* The step out at the level below, so the trunk stays face
             * to face all the way up. */
            x += D8[d][0];
            z += D8[d][1];
            wood(g, x, y - 1, z, LOG_BRANCH);
            stepped++;
        }
        wood(g, x, y, z, LOG_UP);
    }
    put(g, x, h, z, g->leaf);
    put(g, x, h + 1, z, g->leaf);
    int len = g->age == AGE_YOUNG ? 2 : 3;
    for (int k = 0; k < 8; k++) {
        int dx = D8[k][0], dz = D8[k][1];
        if (k & 1) {
            /* The diagonal fronds, shorter. */
            put(g, x + dx, h, z, g->leaf);
            put(g, x + dx, h, z + dz, g->leaf);
            put(g, x + dx, h - 1, z + dz, g->leaf);
            continue;
        }
        /* Up a little from the crown, out, then down at the tip: a frond's
         * arch, within the fronds' span (block.c) of the trunk's top. */
        put(g, x + dx, h + 1, z + dz, g->leaf);
        for (int i = 1; i <= len; i++) put(g, x + dx * i, h, z + dz * i, g->leaf);
        put(g, x + dx * len, h - 1, z + dz * len, g->leaf);
        put(g, x + dx * (len + 1), h - 1, z + dz * (len + 1), g->leaf);
    }
}

/* Baobab: a huge bottle of a trunk, three blocks across, crowned with
 * stubby limbs and sparse tufts of leaves; grows on dry savanna. */
static void baobab(grower *g)
{
    if (g->age == AGE_YOUNG) {
        int h = rand_in(g, 3, 4);
        trunk(g, 0, 0, 0, h);
        spread(g, (pos){0, h - 1, 0}, 3, 1, 1, 1, 1, 1, 40);
        return;
    }
    int h = rand_in(g, 6, 8) + (g->age == AGE_OLD);
    for (int k = 0; k < 9; k++) trunk(g, k % 3 - 1, k / 3 - 1, 0, h);
    /* The trunk narrows to one block at the top. */
    trunk(g, 0, 0, h, h + 1);
    roots(g, -1, -1, 1, 1);
    int n = rand_in(g, 4, 6), start = rand_in(g, 0, 7);
    for (int i = 0; i < n; i++) {
        int d = (start + i * 8 / n) & 7, dx = D8[d][0], dz = D8[d][1];
        pos e = limb(g, (pos){dx, h - 1, dz}, dx, dz, rand_in(g, 1, 2), rand_in(g, 1, 2));
        e.y++;
        blob(g, e, rand_in(g, 1, 2), 1, rand_in(g, 1, 2), 45);
    }
}

/* Umbrella pine: a trunk forking high into a few climbing limbs under a
 * wide, flat, dense parasol of needles. */
static void umbrella(grower *g)
{
    int h = g->age == AGE_YOUNG ? 3 : rand_in(g, 6, 8);
    trunk(g, 0, 0, 0, h + 1);
    int len = g->age == AGE_YOUNG ? 1 : 2, top = h + 2 + (g->age != AGE_YOUNG);
    spread(g, (pos){0, h - 1, 0}, 3, len, 2, 2, 2, 1, 20);
    int r = g->age == AGE_YOUNG ? 2 : 4 + (g->age == AGE_OLD);
    disc(g, (pos){0, top - 1, 0}, r - 2, 20);
    disc(g, (pos){0, top, 0}, r, 25);
    disc(g, (pos){0, top + 1, 0}, r - 1, 30);
}

/* Mountain pine: dwarfed by wind and snow, its trunk short and twisting,
 * its limbs sprawling low with small tufts of needles. */
static void mountain_pine(grower *g)
{
    int h = g->age == AGE_YOUNG ? rand_in(g, 1, 2) : rand_in(g, 2, 4), x = 0, z = 0;
    for (int y = 0; y < h; y++) {
        if (y % 2 == 1) {
            int d = rand_in(g, 0, 3) * 2;
            x += D8[d][0];
            z += D8[d][1];
            wood(g, x, y - 1, z, LOG_BRANCH);
        }
        wood(g, x, y, z, LOG_UP);
    }
    int n = g->age == AGE_YOUNG ? 2 : rand_in(g, 3, 4), start = rand_in(g, 0, 7);
    for (int i = 0; i < n; i++) {
        int d = (start + i * 8 / n) & 7;
        /* From the trunk's last rows: only above its last lean does it
         * stand where (x, z) now is. */
        pos e = limb(g, (pos){x, h - 1 - rand_in(g, 0, h > 1), z}, D8[d][0], D8[d][1], reach(d, rand_in(g, 2, 3)),
                     rand_in(g, 0, 1));
        blob(g, (pos){e.x, e.y + 1, e.z}, rand_in(g, 1, 2), 1, rand_in(g, 1, 2), 35);
    }
    blob(g, (pos){x, h, z}, 1, 1, 1, 20);
}

/* A dead tree: its trunk broken off, a stub of a branch left. */
static void snag(grower *g, int kind)
{
    int w = kind == TREE_REDWOOD ? 2 : 1, h = kind == TREE_REDWOOD ? rand_in(g, 8, 14) : rand_in(g, 3, 6);
    for (int k = 0; k < w * w; k++) trunk(g, k % w, k / w, 0, h - (k == 3));
    int d = rand_in(g, 0, 3) * 2;
    limb(g, (pos){0, h - 2, 0}, D8[d][0], D8[d][1], 1, 0);
}

tree_fall tree_fall_of(uint32_t h)
{
    int d = (int)((h >> 3) & 3u) * 2;
    tree_fall f = {D8[d][0], D8[d][1], 2, 2 + 3 + (int)((h >> 5) % 3u)};
    return f;
}

/* The stump of a fallen tree, its trunk lying on the ground beside it. */
static void fallen(grower *g, uint32_t h)
{
    tree_fall f = tree_fall_of(h);
    trunk(g, 0, 0, 0, rand_in(g, 1, 2));
    for (int i = f.from; i <= f.to; i++) wood(g, f.dx * i, 0, f.dz * i, f.dx ? LOG_X : LOG_Z);
}

/* Drops what the structure solver would (physics.c, physics_check_structure):
 * every solid block needs a path of support to the ground, a step out or
 * down spending a block of the weaker block's span, a step up nothing, and
 * anything resting on a block that stands on the ground standing too.
 * Everything at or below the root's level stands on the ground. */
#define STANDS 127
static void prune(tree_shape *t)
{
    int8_t lvl[TREE_BOX];
    int16_t list[TREE_BOX], stack[TREE_BOX];
    int n = 0;
    for (int i = 0; i < TREE_BOX; i++) {
        lvl[i] = -2;
        if (!(block_get(t->id[i])->flags & BF_SOLID)) continue;
        lvl[i] = i / (TREE_W * TREE_W) - TREE_DOWN <= 0 ? STANDS : -1;
        list[n++] = (int16_t)i;
    }
    static const int N6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int l = STANDS; l >= 0; l = l == STANDS ? STRUCT_MAX_SPAN : l - 1) {
        int sp = 0;
        for (int k = 0; k < n; k++)
            if (lvl[list[k]] == l) stack[sp++] = list[k];
        while (sp > 0) {
            int c = stack[--sp], x = c % TREE_W, z = c / TREE_W % TREE_W, y = c / (TREE_W * TREE_W);
            for (int d = 0; d < 6; d++) {
                int nx = x + N6[d][0], ny = y + N6[d][1], nz = z + N6[d][2];
                if (nx < 0 || nx >= TREE_W || nz < 0 || nz >= TREE_W || ny < 0 || ny >= TREE_H) continue;
                int ni = (ny * TREE_W + nz) * TREE_W + nx;
                if (lvl[ni] < -1) continue;
                int span = block_get(t->id[ni])->span, capped = span < l ? span : l;
                int cand = d == 2 ? (l == STANDS ? STANDS : capped) : capped - 1;
                if (cand > lvl[ni]) {
                    lvl[ni] = (int8_t)cand;
                    if (cand == l) stack[sp++] = (int16_t)ni;
                }
            }
        }
    }
    for (int k = 0; k < n; k++)
        if (lvl[list[k]] == -1) {
            t->id[list[k]] = B_AIR;
            t->meta[list[k]] = 0;
        }
}

/* Vines hang from the underside of the crown, away from the trunk. */
static void hang_vines(grower *g)
{
    tree_shape *t = g->t;
    for (int y = 2; y < TREE_UP; y++)
        for (int z = -TREE_REACH; z <= TREE_REACH; z++)
            for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
                if (abs(x) + abs(z) < 2 || !block_leaves(t->id[tree_cell(x, y, z)])) continue;
                if (t->id[tree_cell(x, y - 1, z)] != B_AIR || !chance(g, 22)) continue;
                int n = rand_in(g, 1, 4);
                for (int k = 1; k <= n && y - k > 0 && t->id[tree_cell(x, y - k, z)] == B_AIR; k++)
                    t->id[tree_cell(x, y - k, z)] = B_VINES;
            }
}

void tree_grow(tree_shape *t, int kind, tree_age age, uint32_t h, int vines)
{
    memset(t->id, B_AIR, sizeof t->id);
    memset(t->meta, 0, sizeof t->meta);
    grower g = {t, h, age, g_tree_log[kind], g_tree_leaves[kind]};
    if (age == AGE_SNAG) snag(&g, kind);
    else if (age == AGE_FALLEN) fallen(&g, h);
    else switch (kind) {
        case TREE_OAK: oak(&g); break;
        case TREE_BIRCH: birch(&g); break;
        case TREE_SPRUCE: spruce(&g); break;
        case TREE_PINE: pine(&g); break;
        case TREE_THORN: thorn(&g); break;
        case TREE_WILLOW: willow(&g); break;
        case TREE_MAPLE: maple(&g); break;
        case TREE_CHERRY: cherry(&g); break;
        case TREE_REDWOOD: redwood(&g); break;
        case TREE_PALM: palm(&g); break;
        case TREE_BAOBAB: baobab(&g); break;
        case TREE_UMBRELLA: umbrella(&g); break;
        default: mountain_pine(&g); break;
        }
    if (kind == TREE_WILLOW && age < AGE_SNAG) curtains(&g);
    prune(t);
    if (vines && age < AGE_SNAG) hang_vines(&g);
    /* Moss on the foot of an old or dead trunk and along a fallen one,
     * where it stays damp. */
    if (age >= AGE_OLD)
        for (int i = 0; i < TREE_BOX; i++) {
            int y = i / (TREE_W * TREE_W) - TREE_DOWN;
            if (block_log(t->id[i]) && y >= 0 && (y < 2 || age == AGE_FALLEN)) t->meta[i] |= LOG_MOSS;
        }
    t->blocks = 0;
    for (int i = 0; i < TREE_BOX; i++) t->blocks += t->id[i] != B_AIR;
}

/* What roots grow through. */
static int soil(uint8_t id)
{
    int m = block_get(id)->mat;
    return id == B_AIR || m == BM_GRASS || m == BM_DIRT || m == BM_SAND;
}

void tree_place(const tree_shape *t, column *c, int x, int y, int z)
{
    int bx = c->cx * CHUNK_W, bz = c->cz * CHUNK_W;
    int x0 = bx - x > -TREE_REACH ? bx - x : -TREE_REACH, x1 = bx + CHUNK_W - 1 - x;
    int z0 = bz - z > -TREE_REACH ? bz - z : -TREE_REACH, z1 = bz + CHUNK_W - 1 - z;
    if (x1 > TREE_REACH) x1 = TREE_REACH;
    if (z1 > TREE_REACH) z1 = TREE_REACH;
    for (int ly = -TREE_DOWN; ly < TREE_UP; ly++) {
        int wy = y + ly;
        if (wy < 1 || wy >= WORLD_H) continue;
        for (int lz = z0; lz <= z1; lz++)
            for (int lx = x0; lx <= x1; lx++) {
                int ti = tree_cell(lx, ly, lz);
                uint8_t id = t->id[ti];
                if (id == B_AIR) continue;
                int ci = col_index(x + lx - bx, wy, z + lz - bz);
                uint8_t *b = &c->blocks[ci];
                int log = block_log(id);
                if (ly < 0 ? !(log && soil(*b)) : !(*b == B_AIR || (log && block_leaves(*b)))) continue;
                *b = id;
                if (t->meta[ti] && !c->meta) c->meta = mem_calloc(COL_VOL, 1);
                if (c->meta) c->meta[ci] = t->meta[ti];
                /* The grass under a trunk dies back to bare earth. */
                uint8_t *under = &c->blocks[ci - COL_AREA];
                if (ly == 0 && log && (*under == B_GRASS || *under == B_PODZOL)) *under = B_DIRT;
            }
    }
}
