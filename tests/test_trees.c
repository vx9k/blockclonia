/* Trees: each kind's shape and how it changes with age, that every tree
 * stands as the structure solver sees it, the extras (snags, fallen
 * trunks, vines, moss), placing a tree across columns, and the faces the
 * mesher gives a log. */
#include "block.h"
#include "inventory.h"
#include "item.h"
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

/* A column of air: column_alloc leaves the blocks to the generator. */
static column *air_column(int cx, int cz)
{
    column *c = column_alloc(cx, cz);
    memset(c->blocks, B_AIR, COL_VOL);
    return c;
}

/* A tree rooted near a column's edge is split between the columns: each
 * keeps its own blocks and together they hold the whole tree. Leaves go
 * only into air, wood also into leaves, roots only into soil, and the
 * grass under the trunk dies back to dirt. */
static void test_tree_place(void)
{
    tree_shape *t = mem_alloc(sizeof *t);
    tree_grow(t, TREE_OAK, AGE_OLD, 5, 0);
    column *c[2] = {air_column(0, 0), air_column(1, 0)};
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
    column *rock = air_column(0, 0);
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

static int cell_x(int i) { return i % TREE_W - TREE_REACH; }
static int cell_z(int i) { return i / TREE_W % TREE_W - TREE_REACH; }
static int cell_y(int i) { return i / (TREE_W * TREE_W) - TREE_DOWN; }

/* How many of a tree's blocks (only its wood, with wood_only) are joined
 * face to face to the ground: to a block on or under it, y <= 0, through
 * blocks of the same sort. Fills reached[] with the cells it got to. */
static int grounded(const tree_shape *t, int wood_only, int *queue, uint8_t *reached)
{
    static const int D[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    memset(reached, 0, TREE_BOX);
    int n = 0;
    for (int i = 0; i < TREE_BOX; i++) {
        uint8_t id = t->id[i];
        if (id == B_AIR || (wood_only && !block_log(id)) || cell_y(i) > 0) continue;
        reached[i] = 1;
        queue[n++] = i;
    }
    for (int h = 0; h < n; h++) {
        int i = queue[h];
        for (int d = 0; d < 6; d++) {
            int x = cell_x(i) + D[d][0], y = cell_y(i) + D[d][1], z = cell_z(i) + D[d][2];
            if (abs(x) > TREE_REACH || abs(z) > TREE_REACH || y < -TREE_DOWN || y >= TREE_UP) continue;
            int j = tree_cell(x, y, z);
            if (reached[j] || t->id[j] == B_AIR || (wood_only && !block_log(t->id[j]))) continue;
            reached[j] = 1;
            queue[n++] = j;
        }
    }
    return n;
}

/* Nothing in any tree floats: every block reaches the ground through the
 * tree, and every log through wood alone, so no limb hangs on its leaves.
 * Only logs carry a meta, and a valid one; roots are limbs; blocks counts
 * them all; and a tree's foot is its trunk. */
static void test_tree_connected(void)
{
    tree_shape *t = mem_alloc(sizeof *t);
    int *queue = mem_alloc(sizeof(int) * TREE_BOX);
    uint8_t *reached = mem_alloc(TREE_BOX);
    int loose = 0, loose_wood = 0, bad_meta = 0, bad_roots = 0, miscount = 0, footless = 0, trees = 0, upright = 0;
    for (int k = 0; k < TREE_KINDS; k++)
        for (int a = AGE_YOUNG; a < AGE_COUNT; a++)
            for (int s = 0; s < 6; s++) {
                tree_grow(t, k, (tree_age)a, sample_hash(s + 40) ^ (uint32_t)(k * 31 + a), s & 1);
                int all = 0, wood = 0;
                for (int i = 0; i < TREE_BOX; i++) {
                    uint8_t id = t->id[i], m = t->meta[i];
                    all += id != B_AIR;
                    wood += block_log(id);
                    bad_meta += m >= FLUID_FULL || (m && !block_log(id)) || (block_log(id) && (m & 3) > LOG_BRANCH);
                    if (cell_y(i) < 0 && id != B_AIR) bad_roots += !block_log(id) || (m & 3) != LOG_BRANCH;
                }
                miscount += all != t->blocks;
                loose += all - grounded(t, 0, queue, reached);
                loose_wood += wood - grounded(t, 1, queue, reached);
                int foot = tree_cell(0, 0, 0);
                footless += !block_log(t->id[foot]);
                upright += block_log(t->id[foot]) && (t->meta[foot] & 3) == LOG_UP;
                trees++;
            }
    CHECK(loose == 0 && loose_wood == 0 && bad_meta == 0 && bad_roots == 0 && miscount == 0 && footless == 0);
    /* A limb may grow back through the foot of a twisted pine; nearly
     * every other foot stands upright. */
    CHECK(upright * 20 >= trees * 19);
    mem_free(reached);
    mem_free(queue);
    mem_free(t);
}

/* Roots: big old trees spread them, buttressed giants have them grown,
 * young trees and the slim birch and spruce never; all of them under the
 * ground and near the trunk. */
static int root_cells(tree_shape *t, int kind, tree_age age, int *far)
{
    int n = 0;
    for (int s = 0; s < 6; s++) {
        tree_grow(t, kind, age, sample_hash(s), 0);
        for (int i = 0; i < TREE_BOX; i++) {
            if (cell_y(i) >= 0 || t->id[i] == B_AIR) continue;
            n++;
            *far += abs(cell_x(i)) > 4 || abs(cell_z(i)) > 4;
        }
    }
    return n;
}

static void test_tree_roots(void)
{
    tree_shape *t = mem_alloc(sizeof *t);
    int far = 0;
    static const int OLD_ROOTS[] = {TREE_OAK, TREE_WILLOW, TREE_MAPLE, TREE_REDWOOD, TREE_BAOBAB};
    for (size_t i = 0; i < sizeof OLD_ROOTS / sizeof OLD_ROOTS[0]; i++)
        CHECK(root_cells(t, OLD_ROOTS[i], AGE_OLD, &far) > 0);
    CHECK(root_cells(t, TREE_REDWOOD, AGE_GROWN, &far) > 0 && root_cells(t, TREE_BAOBAB, AGE_GROWN, &far) > 0);
    CHECK(root_cells(t, TREE_OAK, AGE_GROWN, &far) == 0);
    for (int k = 0; k < TREE_KINDS; k++) CHECK(root_cells(t, k, AGE_YOUNG, &far) == 0);
    for (int a = AGE_YOUNG; a < AGE_COUNT; a++) {
        CHECK(root_cells(t, TREE_BIRCH, (tree_age)a, &far) == 0);
        CHECK(root_cells(t, TREE_SPRUCE, (tree_age)a, &far) == 0);
    }
    CHECK(far == 0);
    /* A redwood's buttresses flare from its foot: wood beside the 2x2
     * trunk, a block or two up. */
    int flare = 0;
    for (int s = 0; s < 6; s++) {
        tree_grow(t, TREE_REDWOOD, AGE_GROWN, sample_hash(s), 0);
        for (int y = 1; y <= 2; y++)
            for (int z = -1; z <= 2; z++)
                for (int x = -1; x <= 2; x++)
                    if ((x < 0 || x > 1 || z < 0 || z > 1) && block_log(t->id[tree_cell(x, y, z)])) flare++;
    }
    CHECK(flare >= 6);
    mem_free(t);
}

/* A fallen trunk lies any of the four ways, starting two blocks out, the
 * same way every time for a hash. */
static void test_tree_fall(void)
{
    int dirs[4] = {0};
    for (uint32_t h = 0; h < 256; h++) {
        tree_fall f = tree_fall_of(h), g = tree_fall_of(h);
        CHECK(f.dx == g.dx && f.dz == g.dz && f.from == g.from && f.to == g.to);
        CHECK(abs(f.dx) + abs(f.dz) == 1 && f.from == 2 && f.to >= f.from + 3 && f.to <= TREE_REACH);
        dirs[f.dx > 0 ? 0 : f.dx < 0 ? 1 : f.dz > 0 ? 2 : 3]++;
    }
    for (int d = 0; d < 4; d++) CHECK(dirs[d] > 32);
}

/* Four columns meeting at the world's origin, x and z negative in three
 * of them: a tree rooted on the corner is shared out four ways, each
 * column taking exactly what lies over it and the four holding the whole
 * tree. */
static void test_tree_corner(void)
{
    tree_shape *t = mem_alloc(sizeof *t);
    tree_grow(t, TREE_OAK, AGE_OLD, 77, 0);
    column *c[4] = {air_column(-1, -1), air_column(0, -1), air_column(-1, 0), air_column(0, 0)};
    const int ground = 30;
    for (int n = 0; n < 4; n++) {
        memset(c[n]->blocks, B_DIRT, (size_t)COL_AREA * ground);
        tree_place(t, c[n], 0, ground, 0);
    }
    int want = 0, got = 0, owners = 0, spread[4] = {0};
    for (int y = -TREE_DOWN; y < TREE_UP; y++)
        for (int z = -TREE_REACH; z <= TREE_REACH; z++)
            for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
                uint8_t id = t->id[tree_cell(x, y, z)];
                int n = (x >= 0) + 2 * (z >= 0);
                int lx = x - c[n]->cx * CHUNK_W, lz = z - c[n]->cz * CHUNK_W;
                CHECK(lx >= 0 && lx < CHUNK_W && lz >= 0 && lz < CHUNK_W);
                uint8_t b = c[n]->blocks[col_index(lx, ground + y, lz)];
                if (id == B_AIR) {
                    CHECK(b == (y < 0 ? B_DIRT : B_AIR));
                    continue;
                }
                want++;
                got += b == id;
                spread[n]++;
            }
    for (int n = 0; n < 4; n++) owners += spread[n] > 0;
    CHECK(want == t->blocks && got == want && owners == 4);
    /* A column off to the side, out of the tree's reach, gets nothing. */
    column *away = air_column(2, 2);
    tree_place(t, away, 0, ground, 0);
    int any = 0;
    for (int i = 0; i < COL_VOL; i++) any += away->blocks[i] != B_AIR;
    CHECK(any == 0 && away->meta == NULL);
    column_free(away);
    for (int n = 0; n < 4; n++) column_free(c[n]);
    mem_free(t);
}

/* What a tree may grow into: its wood through another tree's leaves, its
 * leaves only into air (where crowns meet, the first tree keeps its own),
 * nothing into glass or water; and clipped at the world's floor and
 * ceiling. */
static void test_tree_place_into(void)
{
    tree_shape *t = mem_alloc(sizeof *t);
    tree_grow(t, TREE_SPRUCE, AGE_GROWN, 3, 0);
    column *c = air_column(0, 0);
    const int ground = 20;
    memset(c->blocks, B_DIRT, (size_t)COL_AREA * ground);
    for (int y = ground; y < ground + 12; y++)
        for (int z = 0; z < CHUNK_W; z++)
            for (int x = 0; x < CHUNK_W; x++)
                c->blocks[col_index(x, y, z)] = x < 4 ? B_GLASS : x < 6 ? B_WATER : B_BIRCH_LEAVES;
    tree_place(t, c, 8, ground, 8);
    int logs = 0, kept = 0, glass = 0, water = 0, ours = 0;
    for (int y = 0; y < 12; y++)
        for (int z = -TREE_REACH; z <= TREE_REACH; z++)
            for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
                uint8_t id = t->id[tree_cell(x, y, z)];
                int wx = 8 + x, wz = 8 + z;
                if (id == B_AIR || wx < 0 || wx >= CHUNK_W || wz < 0 || wz >= CHUNK_W) continue;
                uint8_t b = c->blocks[col_index(wx, ground + y, wz)];
                if (wx < 4) glass += b == B_GLASS;
                else if (wx < 6) water += b == B_WATER;
                else if (block_log(id)) logs += b == id;
                else {
                    kept += b == B_BIRCH_LEAVES;
                    ours += b == id;
                }
            }
    CHECK(logs > 4 && kept > 10 && ours == 0 && glass + water > 0);
    for (int i = 0; i < COL_VOL; i++)
        if (c->blocks[i] == B_GLASS || c->blocks[i] == B_WATER) CHECK(i % COL_AREA % CHUNK_W < 6);
    column_free(c);

    /* Near the top of the world the crown is cut off, not wrapped round;
     * on the floor the roots find no ground under row 1. */
    tree_grow(t, TREE_REDWOOD, AGE_OLD, 5, 0);
    column *top = air_column(0, 0), *floor = air_column(0, 0);
    tree_place(t, top, 8, WORLD_H - 6, 8);
    tree_place(t, floor, 8, 1, 8);
    int low = 0, high = 0, under = 0;
    for (int y = 0; y < WORLD_H; y++)
        for (int i = 0; i < COL_AREA; i++) {
            low += y < WORLD_H - 6 - TREE_DOWN && top->blocks[y * COL_AREA + i] != B_AIR;
            high += y >= WORLD_H - 6 && top->blocks[y * COL_AREA + i] != B_AIR;
            under += y < 1 && floor->blocks[y * COL_AREA + i] != B_AIR;
        }
    CHECK(low == 0 && high > 0 && under == 0 && floor->blocks[col_index(8, 1, 8)] == B_REDWOOD_LOG);
    column_free(top);
    column_free(floor);
    mem_free(t);
}

/* The widest reach of a row's leaves from the trunk, or -1 for none. */
static int row_reach(const tree_shape *t, int y)
{
    int r = -1;
    for (int z = -TREE_REACH; z <= TREE_REACH; z++)
        for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
            if (!block_leaves(t->id[tree_cell(x, y, z)])) continue;
            int a = abs(x) > abs(z) ? abs(x) : abs(z);
            r = a > r ? a : r;
        }
    return r;
}

/* Conifers grow their crowns in tiers: going up, the leaves narrow and
 * then spread again at the next whorl, rather than shrinking smoothly to
 * the point. The palm's single crown never does. */
static void test_tree_tiers(void)
{
    tree_shape *t = mem_alloc(sizeof *t);
    long rises[TREE_KINDS] = {0};
    for (int k = 0; k < TREE_KINDS; k++)
        for (int s = 0; s < SAMPLES; s++) {
            tree_grow(t, k, AGE_GROWN, sample_hash(s), 0);
            for (int y = 0, prev = -1; y < TREE_UP; y++) {
                int r = row_reach(t, y);
                if (r < 0) continue;
                rises[k] += prev >= 0 && r > prev;
                prev = r;
            }
        }
    CHECK(rises[TREE_SPRUCE] >= SAMPLES && rises[TREE_REDWOOD] >= SAMPLES);
    CHECK(rises[TREE_PALM] == 0);
    /* An old redwood stacks more tiers than a grown one. */
    long old = 0;
    for (int s = 0; s < SAMPLES; s++) {
        tree_grow(t, TREE_REDWOOD, AGE_OLD, sample_hash(s), 0);
        for (int y = 0, prev = -1; y < TREE_UP; y++) {
            int r = row_reach(t, y);
            if (r < 0) continue;
            old += prev >= 0 && r > prev;
            prev = r;
        }
    }
    CHECK(old >= rises[TREE_REDWOOD]);
    mem_free(t);
}

/* 1 if (x, y, z) is in the tree's box and holds leaves. */
static int leaves_at(const tree_shape *t, int x, int y, int z)
{
    if (abs(x) > TREE_REACH || abs(z) > TREE_REACH || y < -TREE_DOWN || y >= TREE_UP) return 0;
    return block_leaves(t->id[tree_cell(x, y, z)]);
}

static int log_at(const tree_shape *t, int x, int y, int z)
{
    if (abs(x) > TREE_REACH || abs(z) > TREE_REACH || y < -TREE_DOWN || y >= TREE_UP) return 0;
    return block_log(t->id[tree_cell(x, y, z)]);
}

/* Limbs grow out from the trunk and carry leaves: wood two or more blocks
 * from the trunk's axis is common in the spreading kinds, none in a
 * birch, and nearly every limb's tip, the last log going outwards, has
 * leaves within two blocks of it. */
static void test_tree_limbs(void)
{
    tree_shape *t = mem_alloc(sizeof *t);
    long limbs[TREE_KINDS] = {0}, tips = 0, bare = 0;
    for (int k = 0; k < TREE_KINDS; k++)
        for (int s = 0; s < SAMPLES; s++) {
            tree_grow(t, k, AGE_GROWN, sample_hash(s), 0);
            for (int y = 1; y < TREE_UP; y++)
                for (int z = -TREE_REACH; z <= TREE_REACH; z++)
                    for (int x = -TREE_REACH; x <= TREE_REACH; x++) {
                        if (!log_at(t, x, y, z) || (abs(x) < 2 && abs(z) < 2)) continue;
                        limbs[k]++;
                        int sx = x > 0 ? 1 : x < 0 ? -1 : 0, sz = z > 0 ? 1 : z < 0 ? -1 : 0, on = 0, near = 0;
                        for (int dy = -1; dy <= 1; dy++)
                            on |= log_at(t, x + sx, y + dy, z + sz) || log_at(t, x + sx, y + dy, z) ||
                                  log_at(t, x, y + dy, z + sz);
                        if (on) continue;
                        for (int dz = -2; dz <= 2; dz++)
                            for (int dy = -2; dy <= 2; dy++)
                                for (int dx = -2; dx <= 2; dx++) near |= leaves_at(t, x + dx, y + dy, z + dz);
                        tips++;
                        bare += !near;
                    }
        }
    static const int SPREADING[] = {TREE_OAK, TREE_WILLOW, TREE_MAPLE, TREE_CHERRY, TREE_BAOBAB, TREE_UMBRELLA};
    for (size_t i = 0; i < sizeof SPREADING / sizeof SPREADING[0]; i++) CHECK(limbs[SPREADING[i]] >= 4 * SAMPLES);
    CHECK(limbs[TREE_BIRCH] == 0);
    CHECK(tips > 10 * SAMPLES && bare * 20 <= tips);
    mem_free(t);
}

/* What a felled tree is good for, every kind alike: a log splits into four
 * planks anywhere and chars over a fire, comes away whole, and an axe
 * takes it down faster than a hand. Leaves tear into fibre about half the
 * time, and only an oak's give apples. */
static int find_recipe(item_id out, item_id in, int station)
{
    int found = -1;
    for (int r = 0; r < g_recipe_count; r++) {
        const recipe *rc = &g_recipes[r];
        if (rc->out.id == out && rc->in[0].id == in && rc->in[1].id == 0 && rc->station == station)
            found = found < 0 ? r : -2;
    }
    return found;
}

static void test_tree_wood(void)
{
    for (int k = 0; k < TREE_KINDS; k++) {
        uint8_t log = g_tree_log[k], leaf = g_tree_leaves[k];
        int planks = find_recipe(B_PLANKS, log, 0), charcoal = find_recipe(I_CHARCOAL, log, STATION_FIRE);
        CHECK(planks >= 0 && charcoal >= 0);
        if (planks < 0 || charcoal < 0) continue;
        CHECK(g_recipes[planks].out.count == 4 && g_recipes[planks].in[0].count == 1);
        inventory inv = {0};
        inv_init(&inv);
        inv_add(&inv, log, 2);
        CHECK(!inv_can_craft(&inv, charcoal, 0) && inv_can_craft(&inv, charcoal, STATION_FIRE));
        CHECK(inv_craft(&inv, planks, 0) && inv_count(&inv, B_PLANKS) == 4 && inv_count(&inv, log) == 1);
        CHECK(inv_craft(&inv, charcoal, STATION_FIRE) && inv_count(&inv, log) == 0 && inv_count(&inv, I_CHARCOAL) > 0);

        uint32_t rng = 99u + (uint32_t)k;
        item_stack out[3] = {{0}};
        CHECK(item_drops(log, &rng, out) == 1 && out[0].id == log && out[0].count == 1);
        CHECK(block_tool(log) == TOOL_AXE && item_harvests(0, log));
        CHECK(item_dig_speed(item_tool_id(TOOL_AXE, 0), log) > item_dig_speed(0, log));

        int fibre = 0, apples = 0, other = 0;
        for (int i = 0; i < 600; i++) {
            int n = item_drops(leaf, &rng, out);
            for (int j = 0; j < n; j++) {
                fibre += out[j].id == I_FIBRE;
                apples += out[j].id == I_APPLE;
                other += out[j].id != I_FIBRE && out[j].id != I_APPLE;
            }
        }
        CHECK(fibre > 220 && fibre < 380 && other == 0);
        CHECK((apples > 0) == (k == TREE_OAK));
    }
    /* Vines are all strand. */
    uint32_t rng = 5;
    item_stack out[3] = {{0}};
    for (int i = 0; i < 20; i++) CHECK(item_drops(B_VINES, &rng, out) == 1 && out[0].id == I_FIBRE);
}

void test_trees_all(void)
{
    test_tree_shapes();
    test_trees_stand_alone();
    test_tree_extras();
    test_tree_place();
    test_log_faces();
    test_vines_fall();
    test_tree_connected();
    test_tree_roots();
    test_tree_fall();
    test_tree_corner();
    test_tree_place_into();
    test_tree_tiers();
    test_tree_limbs();
    test_tree_wood();
}
