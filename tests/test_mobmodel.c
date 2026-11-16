/* The creatures' looks: the pixel art (mobskin.c) is well formed and
 * covers every face it is drawn on, the figures stand on the ground with
 * their heads in front, and the head, legs and tail move as the mob does. */
#include "block.h"
#include "combat.h"
#include "mem.h"
#include "mob.h"
#include "mobskin.h"
#include "test_util.h"
#include "texgen.h"

#include <math.h>
#include <string.h>

static const float FEET[3] = {0.0f, 0.0f, 0.0f};

static int layer(uint32_t tex, int shift) { return (int)((tex >> shift) & (shift == 24 ? 127u : 255u)); }

/* The lowest point of a box. */
static float bottom(const entity_instance *e)
{
    quat q = {e->rot[0], e->rot[1], e->rot[2], e->rot[3]};
    float low = 1e9f;
    for (int c = 0; c < 8; c++) {
        const float v[3] = {(c & 1 ? 0.5f : -0.5f) * e->scale[0], (c & 2 ? 0.5f : -0.5f) * e->scale[1],
                            (c & 4 ? 0.5f : -0.5f) * e->scale[2]};
        float r[3] = {0};
        quat_apply(q, v, r);
        low = fminf(low, e->pos[1] + r[1]);
    }
    return low;
}

/* The box drawn with this side layer; a failed check and the first box if
 * there is none, so the checks after it still run. */
static const entity_instance *find_box(const entity_instance *e, int n, int side)
{
    for (int i = 0; i < n; i++)
        if (layer(e[i].tex, 0) == side) return &e[i];
    CHECK(side < 0); /* no box with that layer */
    return &e[0];
}

static mob make(int kind)
{
    mob m = {0};
    m.kind = (uint8_t)kind;
    m.body.yaw = 0.0f;
    m.stride = 1.0f;
    return m;
}

static void test_mobskin_art(void)
{
    CHECK(mobskin_check());
    for (int l = T_MOB_FIRST; l <= T_MOB_LAST; l++) {
        int w = 0, h = 0;
        CHECK(mobskin_size(l, &w, &h) && w > 0 && h > 0 && w <= TEX_SIZE && h <= TEX_SIZE);
    }
    int w = 0, h = 0;
    CHECK(!mobskin_size(T_STONE, &w, &h) && w == 0 && h == 0);
    uint8_t px[4] = {0}, edge[4] = {0}, past[4] = {0};
    CHECK(!mobskin_texel(T_STONE, 0, 0, px));
    /* The wolf's amber eye, and a texel past the picture repeats its edge. */
    CHECK(mobskin_texel(T_WOLF_HEAD_FRONT, 1, 2, px) && px[0] > 200 && px[1] > 140 && px[2] < 80 && px[3] == 255);
    CHECK(mobskin_texel(T_WOLF_HEAD_FRONT, 4, 4, edge) && mobskin_texel(T_WOLF_HEAD_FRONT, 15, 15, past));
    CHECK(memcmp(edge, past, 4) == 0);
    /* texgen draws the same picture into the layer. */
    uint8_t tg[4] = {0};
    texgen_texel(T_WOLF_HEAD_FRONT, 1, 2, tg);
    mobskin_texel(T_WOLF_HEAD_FRONT, 1, 2, px);
    CHECK(memcmp(tg, px, 4) == 0);
}

/* Every face of every box is covered by its picture at 16 texels per
 * metre, and no box is longer than one layer. */
static void test_mobmodel_faces(void)
{
    for (int kind = 0; kind < MOB_KINDS; kind++) {
        mob m = make(kind);
        entity_instance e[MOB_FIGURE + 4] = {0};
        CHECK(mob_figure(&m, FEET, 1.0f, 0.0f, e, MOB_FIGURE + 4) == MOB_FIGURE);
        for (int i = 0; i < MOB_FIGURE; i++) {
            CHECK((e[i].tex & ENTITY_TEX_METRIC) != 0);
            float tx[3] = {0};
            for (int k = 0; k < 3; k++) {
                tx[k] = e[i].scale[k] * 16.0f;
                CHECK(tx[k] >= 1.0f && tx[k] <= 16.0f && fabsf(tx[k] - roundf(tx[k])) < 1e-3f);
            }
            int side = layer(e[i].tex, 0), top = layer(e[i].tex, 8), bot = layer(e[i].tex, 16),
                front = layer(e[i].tex, 24);
            if (!front) front = side;
            const int faces[4][3] = {{side, 2, 1}, {top, 0, 2}, {bot, 0, 2}, {front, 0, 1}};
            for (int f = 0; f < 4; f++) {
                int w = 0, h = 0, l = faces[f][0];
                CHECK(l >= T_MOB_FIRST && l <= T_MOB_LAST && mobskin_size(l, &w, &h));
                CHECK((float)w >= roundf(tx[faces[f][1]]) && (float)h >= roundf(tx[faces[f][2]]));
            }
        }
        /* Short of room, it writes what fits and no more. */
        CHECK(mob_figure(&m, FEET, 1.0f, 0.0f, e, 3) == 3);
    }
}

/* On its feet: legs reach the ground at every point of the stride and
 * nothing sinks below it; the head is at the front. */
static void test_mobmodel_pose(void)
{
    for (int kind = 0; kind < MOB_KINDS; kind++) {
        mob m = make(kind);
        entity_instance e[MOB_FIGURE] = {0};
        for (int s = 0; s < 8; s++) {
            m.stride = (float)s * 0.8f;
            int n = mob_figure(&m, FEET, 1.0f, 0.0f, e, MOB_FIGURE);
            float low = 1e9f;
            for (int i = 0; i < n; i++) low = fminf(low, bottom(&e[i]));
            CHECK(low > -0.02f && low < 0.1f);
        }
        int head_l = kind == MOB_WOLF ? T_WOLF_HEAD_SIDE : T_BOAR_HEAD_SIDE;
        int body_l = kind == MOB_WOLF ? T_WOLF_BODY_SIDE : T_BOAR_BODY_SIDE;
        /* Facing -z (yaw 0), then +x (yaw pi/2). */
        m.stride = 0.0f;
        mob_figure(&m, FEET, 1.0f, 0.0f, e, MOB_FIGURE);
        const entity_instance *hd = find_box(e, MOB_FIGURE, head_l), *bd = find_box(e, MOB_FIGURE, body_l);
        CHECK(hd->pos[2] < bd->pos[2] - 0.4f && fabsf(hd->pos[0]) < 1e-4f);
        /* Centred near its feet: the collision box is around them. */
        CHECK(fabsf(bd->pos[2]) < 0.3f);
        m.body.yaw = 1.5707963f;
        mob_figure(&m, FEET, 1.0f, 0.0f, e, MOB_FIGURE);
        hd = find_box(e, MOB_FIGURE, head_l);
        bd = find_box(e, MOB_FIGURE, body_l);
        CHECK(hd->pos[0] > bd->pos[0] + 0.4f);
    }
}

/* The head turns with look and drops with nod; the legs swing with the
 * stride, the tail with the clock; the hurt flash brightens. */
static void test_mobmodel_motion(void)
{
    entity_instance a[MOB_FIGURE] = {0}, b[MOB_FIGURE] = {0};
    mob m = make(MOB_BOAR);
    mob_figure(&m, FEET, 1.0f, 0.0f, a, MOB_FIGURE);
    m.look = 0.6f;
    mob_figure(&m, FEET, 1.0f, 0.0f, b, MOB_FIGURE);
    const entity_instance *h0 = find_box(a, MOB_FIGURE, T_BOAR_HEAD_SIDE),
                          *h1 = find_box(b, MOB_FIGURE, T_BOAR_HEAD_SIDE);
    CHECK(h1->pos[0] > h0->pos[0] + 0.05f); /* towards yaw + look */
    m.look = 0.0f;
    m.nod = 0.9f;
    mob_figure(&m, FEET, 1.0f, 0.0f, b, MOB_FIGURE);
    const entity_instance *s1 = find_box(b, MOB_FIGURE, T_BOAR_SNOUT_SIDE);
    h1 = find_box(b, MOB_FIGURE, T_BOAR_HEAD_SIDE);
    CHECK(h1->pos[1] < h0->pos[1] - 0.05f);
    CHECK(bottom(s1) < 0.2f && bottom(s1) > -0.02f); /* grazing: the snout down by the grass */

    m.nod = 0.0f;
    m.stride = 0.0f;
    mob_figure(&m, FEET, 1.0f, 0.0f, a, MOB_FIGURE);
    m.stride = 1.5f;
    mob_figure(&m, FEET, 1.0f, 0.0f, b, MOB_FIGURE);
    const entity_instance *l0 = find_box(a, MOB_FIGURE, T_BOAR_LEG_LOWER),
                          *l1 = find_box(b, MOB_FIGURE, T_BOAR_LEG_LOWER);
    CHECK(fabsf(l0->pos[2] - l1->pos[2]) > 0.01f);

    mob w = make(MOB_WOLF);
    mob_figure(&w, FEET, 1.0f, 0.0f, a, MOB_FIGURE);
    mob_figure(&w, FEET, 1.0f, 0.7f, b, MOB_FIGURE);
    const entity_instance *t0 = find_box(a, MOB_FIGURE, T_WOLF_TAIL_SIDE),
                          *t1 = find_box(b, MOB_FIGURE, T_WOLF_TAIL_SIDE);
    CHECK(fabsf(t0->pos[0] - t1->pos[0]) > 0.02f);
    w.hurt_t = 0.2f;
    mob_figure(&w, FEET, 1.0f, 0.0f, b, MOB_FIGURE);
    CHECK(b[0].light > a[0].light * 1.5f);
}

/* A step with spawning held off. */
static void step(test_world *t, mobs *ms, combat *c, player *pl, health *h)
{
    ms->spawn_t = 1e9f;
    player *tp[1] = {pl};
    health *th[1] = {h};
    combat_targets tg = {tp, th, 1};
    combat_creatures(c, ms, &t->ph, &tg, 1.0f);
}

/* The head follows a player close by, and a boar left alone grazes. */
static void test_mobmodel_head(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 9);
    combat c = {0};
    combat_init(&c, 9);
    health *h = new_body(4);
    player pl = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);

    /* A boar facing -z with the player 4 m to its right (+x): it glances
     * that way, in the sense of yaw. */
    int b = mobs_add(&ms, MOB_BOAR, dv3(4.5, GROUND, 8.5));
    ms.m[b].think_t = 1e9f;
    ms.m[b].walk = 0.0f;
    ms.m[b].body.yaw = 0.0f;
    for (int i = 0; i < 60; i++) step(&t, &ms, &c, &pl, h);
    CHECK(ms.m[0].look > 0.5f && ms.m[0].look <= 0.9f && ms.m[0].nod < 0.1f);

    /* The player walks off: the head comes back and goes down to graze. */
    pl.pos = dv3(40.5, GROUND, 40.5);
    for (int i = 0; i < 180; i++) step(&t, &ms, &c, &pl, h);
    CHECK(ms.count == 1 && fabsf(ms.m[0].look) < 0.1f && ms.m[0].nod > 0.7f);

    /* A boar on the move does not graze. */
    ms.m[0].walk = 1.0f;
    for (int i = 0; i < 180; i++) step(&t, &ms, &c, &pl, h);
    CHECK(ms.m[0].nod < 0.1f);
    mem_free(h);
    tw_free(&t);
}

/* --mobs: names, and creatures stood in front of the player, facing it. */
static void test_mobmodel_place(void)
{
    CHECK(mobs_kind_named("wolf") == MOB_WOLF && mobs_kind_named("Boar") == MOB_BOAR);
    CHECK(mobs_kind_named("WOLF") == MOB_WOLF);
    CHECK(mobs_kind_named("wolves") == -1 && mobs_kind_named("bo") == -1 && mobs_kind_named("") == -1);

    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 3);
    player pl = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);
    pl.yaw = 0.0f; /* looking along -z */
    int i = mobs_add_ahead(&ms, &t.w, &pl, MOB_WOLF, 4.0, 2.0);
    CHECK(i == 0 && ms.m[0].kind == MOB_WOLF);
    CHECK(fabs(ms.m[0].body.pos.z - (pl.pos.z - 4.0)) < 1e-9 && fabs(ms.m[0].body.pos.x - (pl.pos.x + 2.0)) < 1e-9);
    CHECK(fabs(ms.m[0].body.pos.y - GROUND) < 1e-9);
    /* Facing the player: its forward points back at it. */
    float y = ms.m[0].body.yaw;
    double fx = sin((double)y), fz = -cos((double)y);
    double dx = pl.pos.x - ms.m[0].body.pos.x, dz = pl.pos.z - ms.m[0].body.pos.z;
    CHECK((fx * dx + fz * dz) / sqrt(dx * dx + dz * dz) > 0.999);
    /* Turned a quarter: ahead is now +x. */
    pl.yaw = 1.5707963f;
    i = mobs_add_ahead(&ms, &t.w, &pl, MOB_BOAR, 4.0, 0.0);
    CHECK(i == 1 && fabs(ms.m[1].body.pos.x - (pl.pos.x + 4.0)) < 1e-6 && fabs(ms.m[1].body.pos.z - pl.pos.z) < 1e-6);
    tw_free(&t);
}

static int luma(const uint8_t px[4]) { return px[0] * 3 + px[1] * 6 + px[2]; }

/* The average brightness of a layer's picture. */
static int mean_luma(int l)
{
    int w = 0, h = 0, sum = 0;
    mobskin_size(l, &w, &h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t px[4] = {0};
            mobskin_texel(l, x, y, px);
            sum += luma(px);
        }
    return w > 0 && h > 0 ? sum / (w * h) : 0;
}

/* Every texel of every creature layer, out to the full layer: opaque, the
 * same on every call, and what texgen puts into the texture. */
static void test_mobskin_layers(void)
{
    int mismatched = 0, clear = 0, unstable = 0;
    for (int l = T_MOB_FIRST; l <= T_MOB_LAST; l++)
        for (int y = 0; y < TEX_SIZE; y++)
            for (int x = 0; x < TEX_SIZE; x++) {
                uint8_t a[4] = {0}, b[4] = {0}, g[4] = {0};
                mobskin_texel(l, x, y, a);
                mobskin_texel(l, x, y, b);
                texgen_texel(l, x, y, g);
                unstable += memcmp(a, b, 4) != 0;
                mismatched += memcmp(a, g, 4) != 0;
                clear += a[3] != 255;
            }
    CHECK(mismatched == 0 && clear == 0 && unstable == 0);
    /* Fur varies from texel to texel; the eyes are one flat colour. */
    uint8_t f0[4] = {0}, f1[4] = {0}, f2[4] = {0}, e0[4] = {0}, e1[4] = {0};
    mobskin_texel(T_WOLF_BODY_TOP, 1, 0, f0);
    mobskin_texel(T_WOLF_BODY_TOP, 2, 0, f1);
    mobskin_texel(T_WOLF_BODY_TOP, 3, 0, f2);
    CHECK(memcmp(f0, f1, 4) != 0 || memcmp(f1, f2, 4) != 0);
    mobskin_texel(T_WOLF_HEAD_FRONT, 1, 2, e0);
    mobskin_texel(T_WOLF_HEAD_FRONT, 3, 2, e1);
    CHECK(memcmp(e0, e1, 4) == 0);

    /* The faces read at a glance: a black nose and tail tip on the wolf,
     * dark nostrils between pale tusks on the boar, a cream belly under a
     * dark back, and a boar darker than a wolf. */
    uint8_t nose[4] = {0}, cheek[4] = {0}, tip[4] = {0}, root[4] = {0};
    uint8_t nostril[4] = {0}, disc[4] = {0}, tusk[4] = {0};
    mobskin_texel(T_WOLF_SNOUT_FRONT, 1, 0, nose);
    mobskin_texel(T_WOLF_SNOUT_FRONT, 0, 1, cheek);
    CHECK(luma(nose) * 3 < luma(cheek));
    mobskin_texel(T_WOLF_TAIL_SIDE, 6, 0, tip);
    mobskin_texel(T_WOLF_TAIL_SIDE, 0, 0, root);
    CHECK(luma(tip) * 2 < luma(root));
    mobskin_texel(T_BOAR_SNOUT_FRONT, 1, 1, nostril);
    mobskin_texel(T_BOAR_SNOUT_FRONT, 0, 1, disc);
    mobskin_texel(T_BOAR_SNOUT_FRONT, 0, 2, tusk);
    CHECK(luma(nostril) < luma(disc) && luma(disc) < luma(tusk));
    CHECK(mean_luma(T_WOLF_BELLY) > mean_luma(T_WOLF_BODY_TOP) + 300);
    CHECK(mean_luma(T_BOAR_BODY_SIDE) * 3 < mean_luma(T_WOLF_BODY_SIDE) * 2);
}

/* Standing square on, the figure is mirror-symmetric with four legs; its
 * light follows the light given, and it moves with the feet. */
static void test_mobmodel_symmetry(void)
{
    for (int kind = 0; kind < MOB_KINDS; kind++) {
        mob m = make(kind);
        m.stride = 0.0f;
        entity_instance e[MOB_FIGURE] = {0}, s[MOB_FIGURE] = {0}, dim[MOB_FIGURE] = {0};
        int n = mob_figure(&m, FEET, 1.0f, 0.0f, e, MOB_FIGURE);
        int unmatched = 0, legs = 0;
        int lower = kind == MOB_WOLF ? T_WOLF_LEG_LOWER : T_BOAR_LEG_LOWER;
        for (int i = 0; i < n; i++) {
            /* A trot moves diagonal legs together: a lower leg's twin is on
             * the other side at the other end, at the same height. */
            int leg = layer(e[i].tex, 0) == lower, found = 0;
            legs += leg;
            for (int j = 0; j < n; j++) {
                float dz = fabsf(e[j].pos[2] - e[i].pos[2]);
                found |= e[j].tex == e[i].tex && fabsf(e[j].pos[0] + e[i].pos[0]) < 1e-4f &&
                         fabsf(e[j].pos[1] - e[i].pos[1]) < 1e-4f && (leg ? dz > 0.3f : dz < 1e-4f);
            }
            unmatched += !found;
        }
        CHECK(unmatched == 0 && legs == 4);

        const float shifted[3] = {1.0f, 2.0f, 3.0f};
        mob_figure(&m, shifted, 1.0f, 0.0f, s, MOB_FIGURE);
        mob_figure(&m, FEET, 0.5f, 0.0f, dim, MOB_FIGURE);
        int moved = 0, dimmed = 0;
        for (int i = 0; i < n; i++) {
            moved += fabsf(s[i].pos[0] - e[i].pos[0] - 1.0f) < 1e-4f &&
                     fabsf(s[i].pos[1] - e[i].pos[1] - 2.0f) < 1e-4f && fabsf(s[i].pos[2] - e[i].pos[2] - 3.0f) < 1e-4f;
            dimmed += dim[i].light < e[i].light;
        }
        CHECK(moved == n && dimmed == n);
    }
}

/* The glance has limits and manners: a player behind turns the head only
 * so far, a fleeing boar looks where it runs, and a stalking wolf carries
 * its head low. A creature placed on a raised block stands on top of it. */
static void test_mobmodel_glance(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 11);
    combat c = {0};
    combat_init(&c, 11);
    health *h = new_body(5);
    player pl = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);

    /* The player 4 m behind a boar facing -z, a little to its right. */
    int b = mobs_add(&ms, MOB_BOAR, dv3(8.3, GROUND, 4.5));
    ms.m[b].think_t = 1e9f;
    ms.m[b].walk = 0.0f;
    ms.m[b].body.yaw = 0.0f;
    for (int i = 0; i < 120; i++) step(&t, &ms, &c, &pl, h);
    CHECK(ms.m[0].look > 0.85f && ms.m[0].look <= 0.9f);
    ms.m[0].flee_t = 5.0f;
    for (int i = 0; i < 120; i++) step(&t, &ms, &c, &pl, h);
    CHECK(fabsf(ms.m[0].look) < 0.05f && ms.m[0].nod < 0.05f);

    ms.count = 0;
    int w = mobs_add(&ms, MOB_WOLF, dv3(8.5, GROUND, 24.5));
    ms.m[w].angry_t = 30.0f; /* hunting by day */
    for (int i = 0; i < 30; i++) step(&t, &ms, &c, &pl, h);
    CHECK(ms.count == 1 && ms.m[0].nod > 0.15f && ms.m[0].nod <= 0.3f);

    ms.count = 0;
    pl.yaw = 0.0f;
    CHECK(world_set(&t.w, 8, GROUND, 4, B_STONE, 0));
    int a = mobs_add_ahead(&ms, &t.w, &pl, MOB_BOAR, 4.0, 0.0);
    CHECK(a == 0 && fabs(ms.m[0].body.pos.y - (GROUND + 1)) < 1e-9);
    ms.count = MOB_MAX;
    CHECK(mobs_add_ahead(&ms, &t.w, &pl, MOB_BOAR, 4.0, 0.0) == -1 && ms.count == MOB_MAX);
    mem_free(h);
    tw_free(&t);
}

void test_mobmodel_all(void)
{
    test_mobskin_layers();
    test_mobmodel_symmetry();
    test_mobmodel_glance();
    test_mobskin_art();
    test_mobmodel_faces();
    test_mobmodel_pose();
    test_mobmodel_motion();
    test_mobmodel_head();
    test_mobmodel_place();
}
