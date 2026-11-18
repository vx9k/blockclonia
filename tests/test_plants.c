/* Ground plants in the world: what passes through them (bodies, items,
 * light), what stops at them (the hand's ray), how the hands treat them in
 * each mode, and how the mesher builds them at a section's limits. */
#include "fx.h"
#include "gamemode.h"
#include "interact.h"
#include "inventory.h"
#include "invui.h"
#include "item.h"
#include "light.h"
#include "mem.h"
#include "mesher.h"
#include "test_util.h"
#include "texgen.h"
#include "ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Nothing rests on a plant: a player dropped onto one lands on the soil
 * under it, and so does an item. */
static void test_pass_through(void)
{
    test_world t = {0};
    tw_init(&t);
    for (int x = 2; x < 7; x++)
        for (int z = 2; z < 7; z++) world_set(&t.w, x, GROUND, z, (uint8_t)(B_GRASS_TUFT + (x + z) % 11), 0);
    player p = {0};
    player_spawn(&p, &t.w, 4.5, 4.5);
    p.pos.y = GROUND + 3.0;
    player_input idle = {0};
    for (int i = 0; i < 300 && !p.on_ground; i++) physics_step(&t.ph, &p, &idle);
    CHECK(p.on_ground && fabs(p.pos.y - GROUND) < 1e-6);
    CHECK(world_get(&t.w, 4, GROUND, 4) != B_AIR); /* landing doesn't flatten it */

    p.pos = dv3(40.5, GROUND, 40.5); /* out of reach, so the item stays */
    CHECK(physics_drop_item(&t.ph, dv3(3.5, GROUND + 2.0, 3.5), dv3(0, 0, 0), B_STONE, 1, 10.0f));
    for (int i = 0; i < 240; i++) physics_step(&t.ph, &p, &idle);
    CHECK(t.ph.item_count == 1 && t.ph.items[0].on_ground);
    CHECK(fabs(t.ph.items[0].pos.y - (GROUND + ITEM_HALF)) < 0.01);
    tw_free(&t);
}

/* The hand's ray stops at a plant, so it can be picked, but not at the
 * water beside it. */
static void test_ray_hits(void)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 3, GROUND, 3, B_FERN, 0);
    ray_hit r = physics_raycast(&t.w, dv3(3.5, GROUND + 3.5, 3.5), v3(0.0f, -1.0f, 0.0f), 8.0);
    CHECK(r.hit && r.id == B_FERN && r.block.y == GROUND && r.before.y == GROUND + 1);
    world_set(&t.w, 3, GROUND, 3, B_AIR, 0);
    r = physics_raycast(&t.w, dv3(3.5, GROUND + 3.5, 3.5), v3(0.0f, -1.0f, 0.0f), 8.0);
    CHECK(r.hit && r.id == B_GRASS && r.block.y == GROUND - 1);
    tw_free(&t);
}

static void settle(world *w)
{
    for (int i = 0; i < 200; i++) {
        world_update(w, 0.5, GROUND + 2.0, 0.5);
        jobs_poll(w->jobs, -1);
        world_schedule(w);
    }
}

/* A pit roofed with plants keeps the sky; roofed with dirt it goes dark. */
static void test_light(void)
{
    test_world t = {0};
    tw_init(&t);
    settle(&t.w);
    for (int y = GROUND - 3; y < GROUND; y++) {
        world_set(&t.w, 5, y, 5, B_AIR, 0);
        world_set(&t.w, 9, y, 9, B_AIR, 0);
    }
    world_set(&t.w, 5, GROUND, 5, B_TALL_GRASS, 0);
    world_set(&t.w, 9, GROUND, 9, B_DIRT, 0);
    settle(&t.w);
    CHECK(LIGHT_SKY(world_light(&t.w, 5, GROUND - 3, 5)) == 15);
    CHECK(world_light(&t.w, 9, GROUND - 3, 9) == 0);
    for (int b = B_GRASS_TUFT; b <= B_VINES; b++) CHECK(light_passes((uint8_t)b) && light_emission((uint8_t)b) == 0);
    tw_free(&t);
}

/* A fixture for the hands: a flat world, a player standing south of the
 * origin, empty hands. */
typedef struct {
    test_world t;
    fx_state *fx;
    player pl;
    inventory inv;
    interact ia;
} hands;

static void hands_init(hands *h, int mode)
{
    memset(h, 0, sizeof *h);
    tw_init(&h->t);
    h->fx = mem_calloc(1, sizeof *h->fx);
    fx_init(h->fx, 1);
    h->pl.pos = dv3(4.5, GROUND + 1.0, 1.5);
    h->pl.mode = mode;
    inv_init(&h->inv);
    interact_init(&h->ia, 7);
}

static void hands_free(hands *h)
{
    mem_free(h->fx);
    tw_free(&h->t);
}

static interact_out hands_frame(hands *h, ray_hit hit, int dig)
{
    interact_input in = {.attack = dig, .use_click = !dig, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    vec3 dir = {0.0f, 0.0f, 1.0f};
    return interact_frame(&h->ia, &h->t.w, &h->t.ph, NULL, &h->pl, &h->inv, h->fx, h->pl.pos, dir, hit, &in);
}

/* Breaking by hand: a plant goes in moments, far quicker than its soil. */
static void test_break_fast(void)
{
    hands h = {0};
    hands_init(&h, MODE_SURVIVAL);
    world_set(&h.t.w, 4, GROUND, 4, B_BUSH, 0);
    ray_hit hit = {1, {4, GROUND, 4}, {4, GROUND, 3}, B_BUSH};
    int plant = 0, soil = 0;
    for (; plant < 6000 && world_get(&h.t.w, 4, GROUND, 4) == B_BUSH; plant++) hands_frame(&h, hit, 1);
    hit = (ray_hit){1, {6, GROUND - 1, 4}, {6, GROUND - 1, 3}, B_GRASS};
    for (; soil < 6000 && world_get(&h.t.w, 6, GROUND - 1, 4) == B_GRASS; soil++) hands_frame(&h, hit, 1);
    CHECK(world_get(&h.t.w, 4, GROUND, 4) == B_AIR && plant > 0 && plant * 2 < soil);
    hands_free(&h);
}

/* In creative play a plant uprooted with its soil drops nothing, as the
 * soil itself doesn't. */
static void test_creative_uproot(void)
{
    hands h = {0};
    hands_init(&h, MODE_CREATIVE);
    world_set(&h.t.w, 4, GROUND, 4, B_REEDS, 0);
    ray_hit hit = {1, {4, GROUND - 1, 4}, {4, GROUND - 1, 3}, B_GRASS};
    for (int f = 0; f < 600 && world_get(&h.t.w, 4, GROUND - 1, 4) == B_GRASS; f++) hands_frame(&h, hit, 1);
    CHECK(world_get(&h.t.w, 4, GROUND - 1, 4) == B_AIR && world_get(&h.t.w, 4, GROUND, 4) == B_AIR);
    CHECK(h.t.ph.item_count == 0);
    hands_free(&h);
}

/* Where a plant item takes root: grass, dirt and sand, not stone, snow
 * or planks; and a plant aimed at a plant replaces it in place. */
static void test_roots(void)
{
    hands h = {0};
    hands_init(&h, MODE_SURVIVAL);
    static const struct {
        uint8_t ground;
        int roots;
    } CASES[] = {{B_GRASS, 1}, {B_DIRT, 1}, {B_SAND, 1}, {B_STONE, 0}, {B_SNOW, 0}, {B_PLANKS, 0}};
    for (int i = 0; i < (int)(sizeof CASES / sizeof CASES[0]); i++) {
        int x = 2 + i;
        world_set(&h.t.w, x, GROUND, 5, CASES[i].ground, 0);
        inv_set_held(&h.inv, B_DRY_SHRUB, 1);
        /* Aimed at the block's top, from above: the new plant goes on it. */
        ray_hit hit = {1, {x, GROUND, 5}, {x, GROUND + 1, 5}, CASES[i].ground};
        hands_frame(&h, hit, 0);
        uint8_t got = world_get(&h.t.w, x, GROUND + 1, 5);
        CHECK((got == B_DRY_SHRUB) == CASES[i].roots);
        CHECK((inv_held(&h.inv)->count == 0) == CASES[i].roots);
    }
    /* A poppy aimed at a fern takes its place on the grass; aimed at a
     * fern someone set on stone, it needs soil there as anywhere. */
    world_set(&h.t.w, 10, GROUND, 5, B_FERN, 0);
    world_set(&h.t.w, 12, GROUND - 1, 5, B_STONE, 0);
    world_set(&h.t.w, 12, GROUND, 5, B_FERN, 0);
    inv_set_held(&h.inv, B_POPPY, 2);
    hands_frame(&h, (ray_hit){1, {12, GROUND, 5}, {12, GROUND, 4}, B_FERN}, 0);
    CHECK(world_get(&h.t.w, 12, GROUND, 5) == B_FERN && inv_held(&h.inv)->count == 2);
    hands_frame(&h, (ray_hit){1, {10, GROUND, 5}, {10, GROUND, 4}, B_FERN}, 0);
    CHECK(world_get(&h.t.w, 10, GROUND, 5) == B_POPPY && inv_held(&h.inv)->count == 1);
    hands_free(&h);
}

/* Vines hang from wood and from vines as well as from leaves, so a strand
 * can be grown down, set against the wall beside it (aimed at a vine, a
 * new one would take its place); in creative play the strands come down with their
 * leaf and drop nothing; and every kind's logs lie along the face they
 * are set against. */
static void test_vines_strands(void)
{
    hands h = {0};
    hands_init(&h, MODE_CREATIVE);
    world_set(&h.t.w, 6, GROUND + 4, 6, B_MAPLE_LOG, LOG_BRANCH);
    inv_set_held(&h.inv, B_VINES, 1);
    for (int y = GROUND; y < GROUND + 4; y++) world_set(&h.t.w, 7, y, 6, B_STONE, 0);
    for (int y = GROUND + 3; y > GROUND; y--) hands_frame(&h, (ray_hit){1, {7, y, 6}, {6, y, 6}, B_STONE}, 0);
    for (int y = GROUND + 1; y < GROUND + 4; y++) CHECK(world_get(&h.t.w, 6, y, 6) == B_VINES);
    CHECK(world_get(&h.t.w, 6, GROUND, 6) == B_AIR);

    world_set(&h.t.w, 9, GROUND + 3, 6, B_CHERRY_BLOSSOM, 0);
    world_set(&h.t.w, 9, GROUND + 2, 6, B_VINES, 0);
    world_set(&h.t.w, 9, GROUND + 1, 6, B_VINES, 0);
    ray_hit leaf = {1, {9, GROUND + 3, 6}, {9, GROUND + 3, 5}, B_CHERRY_BLOSSOM};
    for (int f = 0; f < 600 && world_get(&h.t.w, 9, GROUND + 3, 6) != B_AIR; f++) hands_frame(&h, leaf, 1);
    CHECK(world_get(&h.t.w, 9, GROUND + 2, 6) == B_AIR && world_get(&h.t.w, 9, GROUND + 1, 6) == B_AIR);
    CHECK(h.t.ph.item_count == 0);

    for (int k = 0; k < TREE_KINDS; k++) {
        uint8_t log = g_tree_log[k];
        int x = 2 + k;
        world_set(&h.t.w, x, GROUND, 10, B_STONE, 0);
        inv_set_held(&h.inv, log, 2);
        hands_frame(&h, (ray_hit){1, {x, GROUND, 10}, {x, GROUND, 9}, B_STONE}, 0);
        hands_frame(&h, (ray_hit){1, {x, GROUND, 10}, {x, GROUND + 1, 10}, B_STONE}, 0);
        CHECK(world_get(&h.t.w, x, GROUND, 9) == log && world_get_meta(&h.t.w, x, GROUND, 9) == LOG_Z);
        CHECK(world_get(&h.t.w, x, GROUND + 1, 10) == log && world_get_meta(&h.t.w, x, GROUND + 1, 10) == LOG_UP);
    }
    hands_free(&h);
}

/* Vines hang from a tree: placed only under leaves or wood, they come
 * down with what they hang from and stay when the ground under them
 * goes. A log takes the axis of the face it is set against. */
static void test_vines_and_logs(void)
{
    hands h = {0};
    hands_init(&h, MODE_SURVIVAL);
    for (int y = GROUND; y < GROUND + 3; y++) world_set(&h.t.w, 4, y, 4, B_LOG, LOG_UP);
    world_set(&h.t.w, 5, GROUND + 2, 4, B_LEAVES, 0);
    inv_set_held(&h.inv, B_VINES, 2);
    hands_frame(&h, (ray_hit){1, {5, GROUND + 2, 4}, {5, GROUND + 1, 4}, B_LEAVES}, 0);
    CHECK(world_get(&h.t.w, 5, GROUND + 1, 4) == B_VINES && inv_held(&h.inv)->count == 1);
    interact_out o = hands_frame(&h, (ray_hit){1, {10, GROUND - 1, 4}, {10, GROUND, 4}, B_GRASS}, 0);
    CHECK(world_get(&h.t.w, 10, GROUND, 4) == B_AIR && o.msg != NULL && inv_held(&h.inv)->count == 1);

    /* The leaf goes, and both vines under it. */
    world_set(&h.t.w, 5, GROUND, 4, B_VINES, 0);
    ray_hit leaf = {1, {5, GROUND + 2, 4}, {5, GROUND + 2, 3}, B_LEAVES};
    for (int f = 0; f < 6000 && world_get(&h.t.w, 5, GROUND + 2, 4) == B_LEAVES; f++) hands_frame(&h, leaf, 1);
    CHECK(world_get(&h.t.w, 5, GROUND + 1, 4) == B_AIR && world_get(&h.t.w, 5, GROUND, 4) == B_AIR);
    int fibre = 0;
    for (int i = 0; i < h.t.ph.item_count; i++) fibre += h.t.ph.items[i].id == I_FIBRE;
    CHECK(fibre >= 2);

    /* A vine reaching the grass hangs on when the grass is dug out. */
    world_set(&h.t.w, 8, GROUND, 4, B_PLANKS, 0);
    world_set(&h.t.w, 8, GROUND + 1, 4, B_PLANKS, 0);
    world_set(&h.t.w, 7, GROUND + 1, 4, B_LEAVES, 0);
    world_set(&h.t.w, 7, GROUND, 4, B_VINES, 0);
    ray_hit grass = {1, {7, GROUND - 1, 4}, {7, GROUND - 1, 3}, B_GRASS};
    for (int f = 0; f < 6000 && world_get(&h.t.w, 7, GROUND - 1, 4) == B_GRASS; f++) hands_frame(&h, grass, 1);
    CHECK(world_get(&h.t.w, 7, GROUND - 1, 4) == B_AIR && world_get(&h.t.w, 7, GROUND, 4) == B_VINES);

    /* Logs: against the trunk's west side along x, its north side along
     * z, on the ground upright. */
    inv_set_held(&h.inv, B_LOG, 3);
    hands_frame(&h, (ray_hit){1, {4, GROUND, 4}, {3, GROUND, 4}, B_LOG}, 0);
    hands_frame(&h, (ray_hit){1, {4, GROUND + 1, 4}, {4, GROUND + 1, 3}, B_LOG}, 0);
    hands_frame(&h, (ray_hit){1, {12, GROUND - 1, 4}, {12, GROUND, 4}, B_GRASS}, 0);
    CHECK(world_get(&h.t.w, 3, GROUND, 4) == B_LOG && world_get_meta(&h.t.w, 3, GROUND, 4) == LOG_X);
    CHECK(world_get(&h.t.w, 4, GROUND + 1, 3) == B_LOG && world_get_meta(&h.t.w, 4, GROUND + 1, 3) == LOG_Z);
    CHECK(world_get(&h.t.w, 12, GROUND, 4) == B_LOG && world_get_meta(&h.t.w, 12, GROUND, 4) == LOG_UP);
    /* Planks have no axis. */
    inv_set_held(&h.inv, B_PLANKS, 1);
    hands_frame(&h, (ray_hit){1, {12, GROUND, 4}, {13, GROUND, 4}, B_LOG}, 0);
    CHECK(world_get(&h.t.w, 13, GROUND, 4) == B_PLANKS && world_get_meta(&h.t.w, 13, GROUND, 4) == 0);
    hands_free(&h);
}

/* Plants at a section's edges: on its top layer the tip reaches 16, the
 * vertex format's limit; a section of nothing but plants fills the cut-out
 * range with four quads each and stays within the mesher's output. */
static void test_mesh_limits(void)
{
    mesh_input *in = mem_alloc(sizeof *in);
    uint32_t *out = mem_alloc(sizeof(uint32_t) * MESH_OUT_VERTS);
    memset(in->blocks, B_AIR, sizeof in->blocks);
    memset(in->meta, 0, sizeof in->meta);
    in->blocks[mesh_pidx(15, 15, 15)] = B_TALL_GRASS;
    in->blocks[mesh_pidx(0, 0, 0)] = B_HEATHER;
    mesh_counts mc = {0};
    CHECK(mesh_section_counts(in, out, &mc) == 8 && mc.leaf_end == 0 && mc.opaque == 8);
    int top16 = 0, at0 = 0;
    for (int v = 0; v < 32; v++) {
        int x = (int)(out[v] & 31u), y = (int)(out[v] >> 5 & 31u), z = (int)(out[v] >> 10 & 31u);
        top16 += x <= 16 && y == 16 && z <= 16;
        at0 += y == 0;
        CHECK(x <= 16 && y <= 16 && z <= 16);
    }
    CHECK(top16 == 8 && at0 == 8);
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) in->blocks[mesh_pidx(x, y, z)] = (uint8_t)(B_GRASS_TUFT + (x + y + z) % 11);
    CHECK(mesh_section_counts(in, out, &mc) == 16 * 16 * 16 * 4);
    CHECK(mc.opaque == 16 * 16 * 16 * 4 && mc.leaf_end == 0 && mc.trans == 0 && mc.face_end[5] == 0);
    int wrong = 0;
    for (uint32_t q = 0; q < mc.opaque; q++) {
        uint32_t layer = out[4 * q] >> 20 & 0xFFu, face = out[4 * q] >> 15 & 7u;
        wrong += layer < T_PLANT_FIRST || layer > T_PLANT_LAST || face < 6;
    }
    CHECK(wrong == 0);
    /* Plants hide nothing: stone among them keeps all six faces. */
    in->blocks[mesh_pidx(8, 8, 8)] = B_STONE;
    CHECK(mesh_section_counts(in, out, &mc) == (16 * 16 * 16 - 1) * 4 + 6 && mc.face_end[5] == 6);
    mem_free(out);
    mem_free(in);
}

/* A plant's icon is its side texture, flat, filling the slot; the clear
 * texels leave gaps. A cube stays three faces. */
static void test_icon(void)
{
    ui_vertex *mem = mem_alloc(sizeof(ui_vertex) * 4 * 1024);
    ui u;
    ui_begin(&u, mem, 1024, 640, 360);
    invui_icon(&u, B_CORNFLOWER, 10.0f, 20.0f, 32.0f, 1.0f);
    int n = u.quads;
    CHECK(n > 0 && n < 16);
    for (int i = 0; i < 4 * n; i++) {
        CHECK(u.v[i].x >= 10.0f - 0.01f && u.v[i].x <= 42.0f + 0.01f);
        CHECK(u.v[i].y >= 20.0f - 0.01f && u.v[i].y <= 52.0f + 0.01f);
    }
    /* Axis-aligned: every quad's top edge is level. */
    int level = 0;
    for (int i = 0; i < n; i++) level += fabsf(u.v[4 * i].y - u.v[4 * i + 1].y) < 0.01f;
    CHECK(level == n);
    invui_icon(&u, B_STONE, 0.0f, 0.0f, 32.0f, 1.0f);
    CHECK(u.quads - n == 3 * 16);
    mem_free(mem);
}

/* The plants' own marks: reeds' brown heads, the bush's red berries,
 * the heather's pink flowers, all above the plant's foot. */
static void test_marks(void)
{
    static const struct {
        int layer;
        uint8_t r, g, b;
    } MARK[] = {{T_REEDS, 110, 70, 40}, {T_BUSH, 176, 34, 40}, {T_HEATHER, 178, 92, 152}};
    for (int k = 0; k < 3; k++) {
        int near = 0, low = 0;
        for (int y = 0; y < TEX_SIZE; y++)
            for (int x = 0; x < TEX_SIZE; x++) {
                uint8_t t[4] = {0};
                texgen_texel(MARK[k].layer, x, y, t);
                int d = abs(t[0] - MARK[k].r) + abs(t[1] - MARK[k].g) + abs(t[2] - MARK[k].b);
                if (t[3] && d < 60) {
                    near++;
                    low += y == TEX_SIZE - 1;
                }
            }
        CHECK(near >= 1 && low == 0);
    }
    /* Every plant stands on the quad's foot: some texel of the bottom row
     * is solid, so none floats above its soil. Vines hang from the top
     * row instead. */
    for (int l = T_PLANT_FIRST; l <= T_PLANT_LAST; l++) {
        int foot = 0;
        for (int x = 0; x < TEX_SIZE; x++) {
            uint8_t t[4] = {0};
            texgen_texel(l, x, l == T_VINES ? 0 : TEX_SIZE - 1, t);
            foot += t[3] != 0;
        }
        CHECK(foot >= 1);
    }
}

void test_plants_all(void)
{
    test_pass_through();
    test_ray_hits();
    test_light();
    test_break_fast();
    test_creative_uproot();
    test_roots();
    test_vines_and_logs();
    test_vines_strands();
    test_mesh_limits();
    test_icon();
    test_marks();
}
