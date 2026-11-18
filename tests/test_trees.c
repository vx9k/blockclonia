/* Trees: each kind's shape and how it changes with age, that every tree
 * stands as the structure solver sees it, the extras (snags, fallen
 * trunks, vines, moss), placing a tree across columns, and the faces the
 * mesher gives a log. */
#include "block.h"
#include "mem.h"
#include "mesher.h"
#include "physics.h"
#include "test_util.h"
#include "trees.h"

#include <stdlib.h>
#include <string.h>

#define SAMPLES 16

typedef struct {
    long blocks, log_top, leaf_lo, leaf_hi, wide, foot, r_lo, r_hi;
} shape_stats;

static uint32_t sample_hash(int s) { return 0x9E3779B9u * (uint32_t)(s + 1); }

/* Sums over SAMPLES trees of one kind and age: blocks, the top log, the
 * leaves' lowest and highest rows, how far they reach out, the logs one
 * block up (the trunk's girth) and the leaves' reach one row in from the
 * crown's foot and its top. Counts blocks of anything but the kind's own
 * wood and leaves in *alien. */
static shape_stats measure(tree_shape *t, int kind, tree_age age, long *alien)
{
    shape_stats st = {0};
    for (int s = 0; s < SAMPLES; s++) {
        tree_grow(t, kind, age, sample_hash(s), 0);
        int top = -TREE_DOWN, lo = TREE_UP, hi = -TREE_DOWN, wide = 0, r_lo = 0, r_hi = 0;
        for (int y = -TREE_DOWN; y < TREE_UP; y++)
            for (int z = -TREE_REACH; z <= TREE_REACH; z++)
                for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
                    uint8_t id = t->id[tree_cell(x, y, z)];
                    if (id == B_AIR) continue;
                    *alien += id != g_tree_log[kind] && id != g_tree_leaves[kind];
                    if (block_log(id)) {
                        top = y > top ? y : top;
                        st.foot += y == 1;
                        continue;
                    }
                    lo = y < lo ? y : lo;
                    hi = y > hi ? y : hi;
                    int r = abs(x) > abs(z) ? abs(x) : abs(z);
                    wide = r > wide ? r : wide;
                }
        for (int z = -TREE_REACH; z <= TREE_REACH; z++)
            for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
                int r = abs(x) > abs(z) ? abs(x) : abs(z);
                if (lo + 1 < TREE_UP && block_leaves(t->id[tree_cell(x, lo + 1, z)]) && r > r_lo) r_lo = r;
                if (hi > -TREE_DOWN && block_leaves(t->id[tree_cell(x, hi - 1, z)]) && r > r_hi) r_hi = r;
            }
        st.blocks += t->blocks;
        st.log_top += top;
        st.leaf_lo += lo;
        st.leaf_hi += hi;
        st.wide += wide;
        st.r_lo += r_lo;
        st.r_hi += r_hi;
    }
    return st;
}

/* Every kind grows only its own wood and leaves, the same every time from
 * the same hash, bigger with age, and in its own shape. */
static void test_tree_shapes(void)
{
    tree_shape *t = mem_alloc(sizeof *t), *again = mem_alloc(sizeof *again);
    shape_stats st[TREE_KINDS][3] = {{{0}}};
    long alien = 0;
    for (int k = 0; k < TREE_KINDS; k++) {
        for (int a = AGE_YOUNG; a <= AGE_OLD; a++) st[k][a] = measure(t, k, (tree_age)a, &alien);
        CHECK(st[k][AGE_YOUNG].blocks < st[k][AGE_GROWN].blocks && st[k][AGE_GROWN].blocks <= st[k][AGE_OLD].blocks);
        CHECK(st[k][AGE_YOUNG].log_top <= st[k][AGE_GROWN].log_top);
        tree_grow(t, k, AGE_GROWN, 1234, 0);
        tree_grow(again, k, AGE_GROWN, 1234, 0);
        CHECK(memcmp(t->id, again->id, sizeof t->id) == 0 && memcmp(t->meta, again->meta, sizeof t->meta) == 0);
        CHECK(block_log(t->id[tree_cell(0, 0, 0)]) && st[k][AGE_GROWN].leaf_hi > st[k][AGE_GROWN].leaf_lo);
        /* A different hash grows a different tree. */
        tree_grow(again, k, AGE_GROWN, 4321, 0);
        CHECK(memcmp(t->id, again->id, sizeof t->id) != 0);
    }
    CHECK(alien == 0);
    const shape_stats *g = NULL;
    /* The redwood towers over the rest. */
    for (int k = 0; k < TREE_KINDS; k++)
        if (k != TREE_REDWOOD) CHECK(st[TREE_REDWOOD][AGE_GROWN].log_top > 2 * st[k][AGE_GROWN].log_top);
    /* A spruce is a cone, wide at the foot of its crown and a point on top. */
    g = &st[TREE_SPRUCE][AGE_GROWN];
    CHECK(g->r_lo > g->r_hi);
    /* A pine and a palm carry their crowns at the top of a bare trunk. */
    g = &st[TREE_PINE][AGE_GROWN];
    CHECK(g->leaf_lo * 5 > g->log_top * 4);
    g = &st[TREE_PALM][AGE_GROWN];
    CHECK(g->leaf_lo >= g->log_top - SAMPLES);
    /* A willow's leaves hang nearly to the ground. */
    g = &st[TREE_WILLOW][AGE_GROWN];
    CHECK(g->leaf_lo <= 2 * SAMPLES && g->log_top >= 5 * SAMPLES);
    /* A baobab stands on a thick trunk, a birch on a thin one. */
    CHECK(st[TREE_BAOBAB][AGE_GROWN].foot >= 4 * SAMPLES && st[TREE_BIRCH][AGE_GROWN].foot == SAMPLES);
    /* Umbrella and thorn trees are flat-topped, wider than deep. */
    for (int k = TREE_THORN; k <= TREE_UMBRELLA; k += TREE_UMBRELLA - TREE_THORN) {
        g = &st[k][AGE_GROWN];
        CHECK(g->leaf_hi - g->leaf_lo <= 3 * SAMPLES && g->wide >= 4 * SAMPLES);
    }
    /* A birch is narrow, an oak spreads. */
    CHECK(st[TREE_BIRCH][AGE_GROWN].wide <= 3 * SAMPLES && st[TREE_OAK][AGE_GROWN].wide >= 4 * SAMPLES);
    mem_free(again);
    mem_free(t);
}

static void write_tree(world *w, const tree_shape *t, int x0, int y0, int z0, int clear)
{
    for (int y = 0; y < TREE_UP; y++)
        for (int z = -TREE_REACH; z <= TREE_REACH; z++)
            for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
                int i = tree_cell(x, y, z);
                if (t->id[i] != B_AIR) world_set(w, x0 + x, y0 + y, z0 + z, clear ? B_AIR : t->id[i], t->meta[i]);
            }
}

/* Every tree of every kind and age stands as grown on flat ground: the
 * structure solver drops nothing from it. */
static void test_trees_stand_alone(void)
{
    test_world tw = {0};
    tw_init(&tw);
    tree_shape *t = mem_alloc(sizeof *t);
    int fell = 0, trees = 0;
    for (int k = 0; k < TREE_KINDS; k++)
        for (int a = AGE_YOUNG; a < AGE_COUNT; a++)
            for (int s = 0; s < 3; s++) {
                tree_grow(t, k, (tree_age)a, sample_hash(s) ^ (uint32_t)k, 1);
                write_tree(&tw.w, t, 0, GROUND, 0, 0);
                for (int y = GROUND + 12; y < GROUND + TREE_UP; y += 12)
                    fell += physics_check_structure(&tw.ph, 0, y, 0);
                write_tree(&tw.w, t, 0, GROUND, 0, 1);
                trees++;
            }
    CHECK(trees == (int)TREE_KINDS * (int)AGE_COUNT * 3 && fell == 0 && tw.ph.body_count == 0);
    mem_free(t);
    tw_free(&tw);
}

/* A snag is a bare broken trunk, a fallen tree a stump and a trunk lying
 * along the ground where tree_fall_of says, both mossy; vines hang only
 * under leaves or more vine, away from the trunk; old trees are mossy at
 * the foot and grown ones are not. */
static void test_tree_extras(void)
{
    tree_shape *t = mem_alloc(sizeof *t);
    for (int k = 0; k < TREE_KINDS; k++) {
        tree_grow(t, k, AGE_SNAG, 99, 1);
        int leaves = 0, logs = 0, mossy = 0;
        for (int i = 0; i < TREE_BOX; i++) {
            leaves += t->id[i] != B_AIR && !block_log(t->id[i]);
            logs += block_log(t->id[i]);
            mossy += (t->meta[i] & LOG_MOSS) != 0;
        }
        CHECK(leaves == 0 && logs >= 3 && logs < 60 && mossy > 0);

        uint32_t h = 0x51u + (uint32_t)k * 977u;
        tree_grow(t, k, AGE_FALLEN, h, 1);
        tree_fall f = tree_fall_of(h);
        int lying = 0;
        for (int i = f.from; i <= f.to; i++) {
            int c = tree_cell(f.dx * i, 0, f.dz * i);
            lying += block_log(t->id[c]) && (t->meta[c] & 3) == (f.dx ? LOG_X : LOG_Z) && (t->meta[c] & LOG_MOSS);
        }
        CHECK(lying == f.to - f.from + 1 && block_log(t->id[tree_cell(0, 0, 0)]));
        CHECK(abs(f.dx) + abs(f.dz) == 1 && f.from >= 2 && f.to - f.from >= 3 && f.from + f.to <= 2 * TREE_REACH);
    }
    int vines = 0, hanging = 0, near = 0;
    for (int s = 0; s < 8; s++) {
        tree_grow(t, TREE_OAK, AGE_GROWN, sample_hash(s), 1);
        for (int y = 0; y < TREE_UP - 1; y++)
            for (int z = -TREE_REACH; z <= TREE_REACH; z++)
                for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
                    if (t->id[tree_cell(x, y, z)] != B_VINES) continue;
                    uint8_t above = t->id[tree_cell(x, y + 1, z)];
                    vines++;
                    hanging += block_leaves(above) || above == B_VINES;
                    near += abs(x) + abs(z) < 2;
                }
        tree_grow(t, TREE_OAK, AGE_GROWN, sample_hash(s), 0);
        for (int i = 0; i < TREE_BOX; i++) CHECK(t->id[i] != B_VINES);
    }
    CHECK(vines > 8 && hanging == vines && near == 0);
    int old_moss = 0, grown_moss = 0, high_moss = 0;
    for (int s = 0; s < 8; s++) {
        tree_grow(t, TREE_OAK, AGE_OLD, sample_hash(s), 0);
        for (int i = 0; i < TREE_BOX; i++)
            if (t->meta[i] & LOG_MOSS) {
                old_moss++;
                high_moss += i / (TREE_W * TREE_W) - TREE_DOWN >= 2;
            }
        tree_grow(t, TREE_OAK, AGE_GROWN, sample_hash(s), 0);
        for (int i = 0; i < TREE_BOX; i++) grown_moss += (t->meta[i] & LOG_MOSS) != 0;
    }
    CHECK(old_moss >= 8 && high_moss == 0 && grown_moss == 0);
    mem_free(t);
}

/* A tree rooted near a column's edge is split between the columns: each
 * keeps its own blocks and together they hold the whole tree. Leaves go
 * only into air, wood also into leaves, roots only into soil, and the
 * grass under the trunk dies back to dirt. */
static void test_tree_place(void)
{
    tree_shape *t = mem_alloc(sizeof *t);
    tree_grow(t, TREE_OAK, AGE_OLD, 5, 0);
    column *c[2] = {column_alloc(0, 0), column_alloc(1, 0)};
    const int ground = 20, rx = CHUNK_W - 2, rz = 8;
    for (int n = 0; n < 2; n++) {
        for (int y = 0; y < ground; y++)
            for (int z = 0; z < CHUNK_W; z++)
                for (int x = 0; x < CHUNK_W; x++)
                    c[n]->blocks[col_index(x, y, z)] = y == ground - 1 ? B_GRASS : y < ground - 2 ? B_STONE : B_DIRT;
        tree_place(t, c[n], rx, ground, rz);
    }
    int placed = 0, want = 0, wrong = 0, axes = 0;
    for (int y = -TREE_DOWN; y < TREE_UP; y++)
        for (int z = -TREE_REACH; z <= TREE_REACH; z++)
            for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
                int i = tree_cell(x, y, z), wx = rx + x, wz = rz + z, n = wx >= CHUNK_W;
                uint8_t id = t->id[i];
                if (id == B_AIR || wz >= CHUNK_W) continue;
                int ci = col_index(wx - n * CHUNK_W, ground + y, wz);
                /* Roots in dirt only: under the dirt is stone. */
                if (y < 0 && ground + y < ground - 2) {
                    wrong += c[n]->blocks[ci] != B_STONE;
                    continue;
                }
                want++;
                placed += c[n]->blocks[ci] == id;
                if (t->meta[i]) axes += c[n]->meta && c[n]->meta[ci] == t->meta[i];
                else axes++;
            }
    CHECK(want > 100 && placed == want && axes == want && wrong == 0);
    CHECK(c[0]->blocks[col_index(rx, ground - 1, rz)] == B_DIRT &&
          c[0]->blocks[col_index(rx, ground - 1, 0)] == B_GRASS);

    /* Placed over a stone overhang, the leaves stop at the stone. */
    column *rock = column_alloc(0, 0);
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) rock->blocks[col_index(x, ground + 8, z)] = B_STONE;
    tree_place(t, rock, 8, ground, 8);
    int stone = 0;
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) stone += rock->blocks[col_index(x, ground + 8, z)] == B_STONE;
    CHECK(stone == CHUNK_W * CHUNK_W);
    column_free(rock);
    column_free(c[0]);
    column_free(c[1]);
    mem_free(t);
}

/* The layer the mesher gives each face of one log in an empty section,
 * faces in the order +X -X +Y -Y +Z -Z. */
static void log_faces(mesh_input *in, uint32_t *out, uint8_t id, uint8_t meta, int y, int layer[6])
{
    memset(in->blocks, B_AIR, sizeof in->blocks);
    memset(in->meta, 0, sizeof in->meta);
    in->blocks[mesh_pidx(5, y, 7)] = id;
    in->meta[mesh_pidx(5, y, 7)] = meta;
    mesh_counts mc = {0};
    CHECK(mesh_section_counts(in, out, &mc) == 6);
    for (int f = 0; f < 6; f++) layer[f] = (int)(out[4 * f] >> 20 & 0xFFu);
}

/* End grain where a log's axis comes out and nowhere on a limb; upright
 * bark through the four windows of its strip, one block after the next;
 * bark on its side on a lying log; moss where the meta says. */
static void test_log_faces(void)
{
    mesh_input *in = mem_alloc(sizeof *in);
    uint32_t *out = mem_alloc(sizeof(uint32_t) * MESH_OUT_VERTS);
    int l[6] = {0}, k = TREE_MAPLE;
    uint8_t log = g_tree_log[k];
    log_faces(in, out, log, LOG_UP, 3, l);
    int w = l[0] - tree_tex_bark(k, 0);
    CHECK(w >= 0 && w < TREE_BARK_WINDOWS && l[1] == l[0] && l[4] == l[0] && l[5] == l[0]);
    CHECK(l[2] == tree_tex_end(k) && l[3] == tree_tex_end(k));
    for (int y = 4; y < 12; y++) {
        log_faces(in, out, log, LOG_UP, y, l);
        CHECK(l[0] == tree_tex_bark(k, (w + y - 3) % TREE_BARK_WINDOWS));
    }
    log_faces(in, out, log, LOG_X, 3, l);
    CHECK(l[0] == tree_tex_end(k) && l[1] == tree_tex_end(k) && l[2] == tree_tex_sideways(k) &&
          l[4] == tree_tex_sideways(k));
    log_faces(in, out, log, LOG_Z, 3, l);
    CHECK(l[4] == tree_tex_end(k) && l[5] == tree_tex_end(k) && l[0] == tree_tex_sideways(k) &&
          l[2] == tree_tex_sideways(k));
    log_faces(in, out, log, LOG_BRANCH, 3, l);
    for (int f = 0; f < 6; f++) CHECK(l[f] == tree_tex_sideways(k));
    log_faces(in, out, log, LOG_UP | LOG_MOSS, 3, l);
    CHECK(l[0] == tree_tex_mossy(k) && l[5] == tree_tex_mossy(k) && l[2] == tree_tex_end(k));
    log_faces(in, out, log, LOG_X | LOG_MOSS, 3, l);
    CHECK(l[2] == tree_tex_mossy(k) && l[3] == tree_tex_sideways(k) && l[0] == tree_tex_end(k));
    /* Planks are not a log: their own texture all round, whatever the meta. */
    log_faces(in, out, B_PLANKS, 0, 3, l);
    CHECK(l[0] == block_get(B_PLANKS)->tex[0]);
    mem_free(out);
    mem_free(in);
}

/* A collapse brings the vines hanging from it down too. */
static void test_vines_fall(void)
{
    test_world tw = {0};
    tw_init(&tw);
    world_set(&tw.w, 6, GROUND + 4, 6, B_LEAVES, 0);
    world_set(&tw.w, 6, GROUND + 3, 6, B_VINES, 0);
    world_set(&tw.w, 6, GROUND + 2, 6, B_VINES, 0);
    world_set(&tw.w, 9, GROUND + 1, 6, B_VINES, 0); /* under nothing it holds on to; not this collapse's */
    CHECK(physics_check_structure(&tw.ph, 6, GROUND + 4, 6) == 1);
    CHECK(world_get(&tw.w, 6, GROUND + 4, 6) == B_AIR && world_get(&tw.w, 6, GROUND + 3, 6) == B_AIR &&
          world_get(&tw.w, 6, GROUND + 2, 6) == B_AIR && world_get(&tw.w, 9, GROUND + 1, 6) == B_VINES);
    tw_free(&tw);
}

void test_trees_all(void)
{
    test_tree_shapes();
    test_trees_stand_alone();
    test_tree_extras();
    test_tree_place();
    test_log_faces();
    test_vines_fall();
}
