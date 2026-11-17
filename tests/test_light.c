/* Light: flood fill, storage, the shader's texels, the glow curve and
 * the light sources' items. */
#include "block.h"
#include "inventory.h"
#include "item.h"
#include "jobs.h"
#include "light.h"
#include "mem.h"
#include "mesher.h"
#include "test_util.h"
#include "texgen.h"

#include <math.h>
#include <string.h>

#define R LIGHT_REGION
#define FOOT_VOL ((size_t)LIGHT_FOOT * LIGHT_FOOT * WORLD_H)

static size_t ridx(int x, int y, int z) { return ((size_t)y * R + (size_t)z) * R + (size_t)x; }

/* Snapshot coordinates of a block of the middle column. */
static size_t mid(int x, int y, int z) { return ridx(CHUNK_W + x, y, CHUNK_W + z); }

static uint16_t foot_at(const uint16_t *foot, int x, int y, int z)
{
    size_t i = (size_t)y * LIGHT_FOOT * LIGHT_FOOT + (size_t)((z + 1) * LIGHT_FOOT + (x + 1));
    return foot[i];
}

/* Stone up to y = 19, air above. */
static uint8_t *flat_region(void)
{
    uint8_t *b = mem_calloc((size_t)R * R * WORLD_H, 1);
    memset(b, B_STONE, (size_t)R * R * 20);
    return b;
}

static void test_sky(void)
{
    uint8_t *b = flat_region();
    uint16_t *foot = mem_alloc(FOOT_VOL * sizeof *foot);
    light_region(b, 21, NULL, 0, foot);
    CHECK(foot_at(foot, 5, 20, 5) == LIGHT_OPEN_SKY); /* on the ground */
    CHECK(foot_at(foot, 5, 300, 5) == LIGHT_OPEN_SKY); /* above the snapshot's top */
    CHECK(foot_at(foot, -1, 20, 16) == LIGHT_OPEN_SKY); /* the border too */
    CHECK(foot_at(foot, 5, 19, 5) == 0);               /* inside the stone */

    /* A sealed room 10 blocks down is dark. */
    for (int y = 8; y <= 10; y++)
        for (int z = 4; z <= 8; z++)
            for (int x = 4; x <= 8; x++) b[mid(x, y, z)] = B_AIR;
    light_region(b, 21, NULL, 0, foot);
    CHECK(foot_at(foot, 6, 9, 6) == 0);

    /* A roof over open ground: shade falls off one level per block in
     * from the edge, and straight under the middle of a wide roof it is
     * darker still. */
    memset(b, B_STONE, (size_t)R * R * 20);
    for (int z = -16; z < 32; z++)
        for (int x = 0; x < 32; x++) b[mid(x, 24, z)] = B_STONE; /* x from 0 on: open to the west */
    light_region(b, 25, NULL, 0, foot);
    CHECK(LIGHT_SKY(foot_at(foot, -1, 22, 5)) == 15); /* west of the roof */
    CHECK(LIGHT_SKY(foot_at(foot, 0, 22, 5)) == 14);
    CHECK(LIGHT_SKY(foot_at(foot, 3, 22, 5)) == 11);
    CHECK(LIGHT_SKY(foot_at(foot, 14, 22, 5)) == 0);
    CHECK(LIGHT_SKY(foot_at(foot, 3, 25, 5)) == 15); /* on the roof */
    mem_free(foot);
    mem_free(b);
}

static void test_emitters(void)
{
    uint8_t *b = flat_region();
    uint16_t *foot = mem_alloc(FOOT_VOL * sizeof *foot);
    /* A long tunnel at y = 10 with a lamp at x = 0. */
    for (int x = -15; x < 32; x++) b[mid(x, 10, 5)] = B_AIR;
    b[mid(0, 10, 5)] = B_LAMP;
    light_region(b, 21, NULL, 0, foot);
    uint16_t e = light_emission(B_LAMP);
    CHECK(e == LIGHT_PACK(0, 15, 13, 8));
    CHECK(foot_at(foot, 0, 10, 5) == e);
    CHECK(foot_at(foot, 1, 10, 5) == LIGHT_PACK(0, 14, 12, 7));
    CHECK(foot_at(foot, 5, 10, 5) == LIGHT_PACK(0, 10, 8, 3));
    CHECK(foot_at(foot, 10, 10, 5) == LIGHT_PACK(0, 5, 3, 0)); /* blue runs out first */
    CHECK(foot_at(foot, 15, 10, 5) == 0);
    CHECK(foot_at(foot, 16, 10, 5) == 0); /* the border, 16 blocks off */
    CHECK(foot_at(foot, 0, 11, 5) == 0);  /* no light inside the rock around it */
    CHECK(foot_at(foot, 0, 10, 7) == 0);  /* nor through it into a parallel tunnel */

    /* Two lamps of different colours: per channel, the brighter wins. */
    b[mid(10, 10, 5)] = B_FOXFIRE;
    light_region(b, 21, NULL, 0, foot);
    uint16_t fox = light_emission(B_FOXFIRE);
    CHECK(foot_at(foot, 10, 10, 5) == fox); /* opaque: the lamp's light stops at it */
    CHECK(foot_at(foot, 11, 10, 5) == LIGHT_PACK(0, 1, 6, 3));
    b[mid(10, 10, 5)] = B_AIR;
    b[mid(11, 10, 5)] = B_TORCH;
    light_region(b, 21, NULL, 0, foot);
    CHECK(foot_at(foot, 10, 10, 5) == LIGHT_PACK(0, 13, 9, 4)); /* the nearer torch outshines the lamp */

    /* A hot cell given from outside glows like a block. */
    memset(b + (size_t)R * R * 10, B_STONE, (size_t)R * R);
    for (int x = -15; x < 32; x++) b[mid(x, 10, 5)] = B_AIR;
    light_emitter hot = {(int16_t)(CHUNK_W + 3), 10, (int16_t)(CHUNK_W + 5), light_glow(900.0f)};
    light_region(b, 21, &hot, 1, foot);
    CHECK(foot_at(foot, 3, 10, 5) == light_glow(900.0f));
    CHECK(LIGHT_R(foot_at(foot, 4, 10, 5)) == LIGHT_R(light_glow(900.0f)) - 1);
    /* Out of the snapshot or above its top: ignored, not a crash. */
    light_emitter bad[2] = {{-1, 10, 5, 0xFFF0}, {5, 400, 5, 0xFFF0}};
    light_region(b, 21, bad, 2, foot);
    CHECK(foot_at(foot, 3, 10, 5) == 0);

    /* Lava lights the cave it lies in; a campfire too. */
    CHECK(LIGHT_R(light_emission(B_LAVA)) == 15 && LIGHT_B(light_emission(B_LAVA)) < LIGHT_G(light_emission(B_LAVA)));
    CHECK(light_emission(B_CAMPFIRE) != 0 && light_emission(B_TORCH) != 0);
    CHECK(light_emission(B_STONE) == 0 && light_emission(B_AIR) == 0 && light_emission(B_FURNACE) == 0);
    mem_free(foot);
    mem_free(b);
}

/* Two neighbouring columns computed from their own snapshots agree where
 * their footprints overlap, so a face on the border is lit the same from
 * either side. */
static uint8_t terrain(int x, int y, int z)
{
    /* Rolling ground with an overhanging ledge, a cave and some lamps. */
    int h = 30 + (int)(6.0 * sin(x * 0.21) + 5.0 * cos(z * 0.17));
    if (y == 36 && x > 10 && x < 30 && z > 0 && z < 20) return B_STONE;
    if (y > 14 && y < 18 && (x + z) % 23 < 5) return y == 15 && (x * 7 + z) % 11 == 0 ? B_TORCH : B_AIR;
    if (y < h) return (x * 13 + y * 7 + z * 3) % 97 == 0 ? B_LAMP : B_STONE;
    return B_AIR;
}

static void fill(uint8_t *b, int cx, int cz)
{
    for (int y = 0; y < 64; y++)
        for (int z = 0; z < R; z++)
            for (int x = 0; x < R; x++) b[ridx(x, y, z)] = terrain((cx - 1) * CHUNK_W + x, y, (cz - 1) * CHUNK_W + z);
}

static void test_borders(void)
{
    uint8_t *b = mem_calloc((size_t)R * R * WORLD_H, 1);
    uint16_t *f0 = mem_alloc(FOOT_VOL * sizeof *f0), *f1 = mem_alloc(FOOT_VOL * sizeof *f1);
    fill(b, 0, 0);
    light_region(b, 64, NULL, 0, f0);
    fill(b, 1, 0);
    light_region(b, 64, NULL, 0, f1);
    int same = 1, lit = 0, dark = 0;
    for (int y = 0; y < 64; y++)
        for (int z = -1; z <= CHUNK_W; z++) {
            same &= foot_at(f0, 15, y, z) == foot_at(f1, -1, y, z);
            same &= foot_at(f0, 16, y, z) == foot_at(f1, 0, y, z);
            lit += LIGHT_R(foot_at(f0, 16, y, z)) > 0;
            dark += LIGHT_SKY(foot_at(f0, 16, y, z)) > 0 && LIGHT_SKY(foot_at(f0, 16, y, z)) < 15;
        }
    CHECK(same);
    CHECK(lit > 0 && dark > 0); /* the border saw both block light and shade */
    /* The same along z. */
    fill(b, 0, 1);
    light_region(b, 64, NULL, 0, f1);
    same = 1;
    for (int y = 0; y < 64; y++)
        for (int x = -1; x <= CHUNK_W; x++) same &= foot_at(f0, x, y, 16) == foot_at(f1, x, y, 0);
    CHECK(same);
    mem_free(f0);
    mem_free(f1);
    mem_free(b);
}

static void test_store(void)
{
    uint8_t *b = flat_region();
    uint16_t *foot = mem_alloc(FOOT_VOL * sizeof *foot);
    b[mid(4, 21, 4)] = B_TORCH;
    light_region(b, 22, NULL, 0, foot);
    column_light l = {0};
    uint64_t ch = light_store(foot, &l);
    CHECK((ch & 1) == 0 && (ch & 2) && (ch & 1ull << 5)); /* dark rock was already 0 */
    CHECK(l.cells[0] == NULL && l.fill[0] == 0);            /* solid stone */
    CHECK(l.cells[1] != NULL);                              /* ground, sky and the torch */
    CHECK(l.cells[5] == NULL && l.fill[5] == LIGHT_OPEN_SKY);
    CHECK(light_get(&l, 4, 21, 4) == (light_emission(B_TORCH) | 15u));
    CHECK(light_get(&l, 4, 20, 4) == foot_at(foot, 4, 20, 4));
    CHECK(light_get(&l, 0, -1, 0) == 0 && light_get(&l, 0, WORLD_H, 0) == LIGHT_OPEN_SKY);
    CHECK(light_store(foot, &l) == 0); /* nothing changed */
    b[mid(4, 21, 4)] = B_AIR;
    light_region(b, 22, NULL, 0, foot);
    CHECK(light_store(foot, &l) == 1ull << 1); /* only the torch's section */
    CHECK(l.cells[1] != NULL);                  /* still stone below, sky above */
    light_free(&l);
    CHECK(l.cells[1] == NULL);
    mem_free(foot);
    mem_free(b);
}

static void test_texels(void)
{
    uint8_t *b = flat_region();
    uint16_t *foot = mem_alloc(FOOT_VOL * sizeof *foot);
    b[mid(4, 21, 4)] = B_TORCH;
    light_region(b, 22, NULL, 0, foot);
    column_light l = {0};
    light_store(foot, &l);
    mesh_input *in = mem_calloc(1, sizeof *in);
    for (int y = -1; y <= 16; y++)
        for (int z = -1; z <= 16; z++)
            for (int x = -1; x <= 16; x++) in->blocks[mesh_pidx(x, y, z)] = b[mid(x, 16 + y, z)];
    uint16_t *tex = mem_alloc(LIGHT_TEX_VOL * sizeof *tex), u = 0;
    light_copy_texels(&l, 1, tex);
    CHECK(!light_fix_texels(in->blocks, tex, &u));
    /* An open cell is its own light; the stone under the torch takes the
     * torch's, so the filter doesn't darken the edge of the lit floor. */
    CHECK(tex[mesh_pidx(4, 5, 4)] == light_get(&l, 4, 21, 4));
    CHECK(tex[mesh_pidx(4, 3, 4)] == light_get(&l, 4, 20, 4)); /* the floor takes the air above it */
    CHECK(tex[mesh_pidx(4, 1, 4)] == 0);                        /* deep in the stone, no open neighbour */
    CHECK(tex[mesh_pidx(-1, 5, 4)] == light_get(&l, -1, 21, 4));
    /* A one-block wall between the open sky and a shut room takes the
     * room's dark, not the sky, so it can't leak into the room's corners. */
    memset(in->blocks, B_STONE, sizeof in->blocks);
    for (int i = 0; i < LIGHT_TEX_VOL; i++) tex[i] = LIGHT_OPEN_SKY;
    in->blocks[mesh_pidx(4, 5, 4)] = B_AIR;
    tex[mesh_pidx(4, 5, 4)] = 0;                       /* the room */
    in->blocks[mesh_pidx(6, 5, 4)] = B_AIR;            /* the outside */
    in->blocks[mesh_pidx(8, 5, 4)] = B_AIR;
    tex[mesh_pidx(8, 5, 4)] = LIGHT_PACK(3, 9, 0, 0);
    CHECK(!light_fix_texels(in->blocks, tex, &u));
    CHECK(tex[mesh_pidx(5, 5, 4)] == 0);
    CHECK(tex[mesh_pidx(7, 5, 4)] == LIGHT_PACK(3, 0, 0, 0)); /* level by level: sky 3 of 15, red 0 of 9 */
    /* Open sky throughout: one value. */
    memset(in->blocks, B_AIR, sizeof in->blocks);
    light_copy_texels(&l, 6, tex);
    CHECK(light_fix_texels(in->blocks, tex, &u) && u == LIGHT_OPEN_SKY);
    light_free(&l);
    mem_free(tex);
    mem_free(in);
    mem_free(foot);
    mem_free(b);
}

static void test_curves(void)
{
    /* The shader's curve and its daylight shift (block.frag) agree with
     * light_brightness at every sky level and a dusk daylight. */
    CHECK(fabsf(LIGHT_CURVE_K + 15.0f * log2f(0.8f)) < 1e-5f);
    float shift = log2f(0.3f) / LIGHT_CURVE_K, worst = 0.0f;
    for (unsigned s = 1; s <= 15; s++) {
        float shader = exp2f(((float)s / 15.0f + shift) * LIGHT_CURVE_K - LIGHT_CURVE_K);
        float cpu = light_brightness(LIGHT_PACK(s, 0, 0, 0), 0.3f);
        worst = fmaxf(worst, fabsf(shader - cpu) / cpu);
    }
    CHECK(worst < 1e-4f);
    CHECK(light_glow(20.0f) == 0 && light_glow(499.0f) == 0);
    CHECK(LIGHT_R(light_glow(550.0f)) >= 1 && LIGHT_G(light_glow(550.0f)) == 0);
    CHECK(LIGHT_SKY(light_glow(1100.0f)) == 0 && LIGHT_R(light_glow(1100.0f)) == 15);
    unsigned last = 0;
    int rising = 1;
    for (int k = 0; k <= 30; k++) {
        uint16_t g = light_glow(500.0f + 50.0f * (float)k);
        unsigned sum = LIGHT_R(g) + LIGHT_G(g) + LIGHT_B(g);
        rising &= sum >= last && LIGHT_R(g) >= LIGHT_G(g) && LIGHT_G(g) >= LIGHT_B(g);
        last = sum;
    }
    CHECK(rising && last == 45); /* white hot */

    CHECK(fabsf(light_brightness(LIGHT_OPEN_SKY, 1.0f) - 1.0f) < 1e-6f);
    CHECK(fabsf(light_brightness(LIGHT_OPEN_SKY, 0.18f) - 0.18f) < 1e-6f);
    CHECK(fabsf(light_brightness(LIGHT_PACK(14, 0, 0, 0), 1.0f) - 0.8f) < 1e-6f);
    CHECK(light_brightness(0, 1.0f) < 0.04f);
    /* A torch keeps a room lit at night. */
    CHECK(light_brightness(LIGHT_PACK(15, 14, 10, 5), 0.18f) > 0.75f);
    CHECK(light_passes(B_AIR) && light_passes(B_GLASS) && light_passes(B_WATER) && light_passes(B_TORCH));
    CHECK(!light_passes(B_STONE) && !light_passes(B_FOXFIRE));
}

static void test_atlas(void)
{
    int n[3] = {0};
    CHECK(light_atlas_layout(2048, 2000, n) == 2156 && n[0] == 14 && n[1] == 14 && n[2] == 11);
    CHECK(light_atlas_layout(2048, 3000, n) == 3136 && n[0] == 28 && n[2] == 4);
    CHECK(light_atlas_layout(2048, 300, n) == 343 && n[0] == 7 && n[2] == 7);
    CHECK(light_atlas_layout(100, 300, n) == 125 && n[0] == 5 && n[2] == 5); /* under 7 a side */
    /* The smallest 3D image Vulkan allows: 14 slots a side, whatever is wanted. */
    CHECK(light_atlas_layout(256, 5000, n) == 2744 && n[2] == 14);
    /* The packing's limits hold on a device that allows more. */
    CHECK(light_atlas_layout(16384, 100000, n) >= 100000 && n[0] * LIGHT_TEX <= LIGHT_ATLAS_XY_MAX);
    CHECK(light_atlas_layout(16384, 10000000, n) == 112 * 112 * 56 && n[2] * LIGHT_TEX <= LIGHT_ATLAS_Z_MAX);
    CHECK(light_atlas_layout(10, 0, n) == 1); /* degenerate, still one slot */
    int m[3] = {13, 13, 12};
    CHECK(light_slot_base(0, m) == 0 && light_slot_base(1, m) == 18u && light_slot_base(13, m) == 18u << 11);
    CHECK(light_slot_base(169, m) == 18u << 22);
    CHECK(light_slot_base(13 * 13 * 12 - 1, m) == (216u | 216u << 11 | 198u << 22));
}

static void settle(world *w)
{
    for (int i = 0; i < 200; i++) {
        world_update(w, 0.5, GROUND + 2.0, 0.5);
        jobs_poll(w->jobs, -1);
        world_schedule(w);
    }
}

/* The world keeps every column's light up to date as blocks change,
 * across column borders too. */
static void test_world_light(void)
{
    test_world t = {0};
    tw_init(&t);
    /* Not lit yet: open sky, so nothing turns black while streaming. */
    CHECK(world_light(&t.w, 3, 2, 3) == LIGHT_OPEN_SKY);
    CHECK(light_at(&t.w, 3.5, 2.5, 3.5, 0.5f) == 0.5f);
    settle(&t.w);
    const column *c = world_column(&t.w, 0, 0);
    CHECK(c && c->lit && !c->light_dirty && !c->lighting);
    CHECK(world_light(&t.w, 3, GROUND, 3) == LIGHT_OPEN_SKY);
    CHECK(world_light(&t.w, 3, GROUND - 2, 3) == 0);
    /* A sealed pit is dark; a torch in it lights it. */
    for (int y = GROUND - 3; y < GROUND; y++) world_set(&t.w, 5, y, 5, B_AIR, 0);
    world_set(&t.w, 5, GROUND, 5, B_STONE, 0);
    settle(&t.w);
    CHECK(world_light(&t.w, 5, GROUND - 2, 5) == 0);
    world_set(&t.w, 5, GROUND - 3, 5, B_TORCH, 0);
    settle(&t.w);
    uint16_t e = light_emission(B_TORCH);
    CHECK(world_light(&t.w, 5, GROUND - 3, 5) == e);
    CHECK(LIGHT_R(world_light(&t.w, 5, GROUND - 1, 5)) == LIGHT_R(e) - 2);
    /* Taking the roof off lets the sky straight down. */
    world_set(&t.w, 5, GROUND, 5, B_AIR, 0);
    settle(&t.w);
    CHECK(LIGHT_SKY(world_light(&t.w, 5, GROUND - 3, 5)) == 15);
    /* A torch on a column's edge lights the next column. */
    world_set(&t.w, 15, GROUND, 8, B_TORCH, 0);
    settle(&t.w);
    CHECK(LIGHT_R(world_light(&t.w, 15, GROUND, 8)) == LIGHT_R(e));
    CHECK(LIGHT_R(world_light(&t.w, 16, GROUND, 8)) == LIGHT_R(e) - 1);
    CHECK(LIGHT_R(world_light(&t.w, 20, GROUND, 8)) == LIGHT_R(e) - 5);
    /* An edit that changes no light schedules no work. */
    world_set(&t.w, 40, GROUND - 2, 40, B_STONE, 0);
    CHECK(!world_column(&t.w, 2, 2)->light_dirty);
    tw_free(&t);
}

/* Which blocks pass light and which give it off, and the glow colours. */
static void test_blocks(void)
{
    int bad = 0, emitters = 0;
    for (int id = 0; id < B_COUNT; id++) {
        uint16_t e = light_emission((uint8_t)id);
        bad += light_passes((uint8_t)id) != !block_opaque((uint8_t)id);
        bad += LIGHT_SKY(e) != 0; /* block light never brings sky */
        emitters += e != 0;
    }
    CHECK(bad == 0);
    CHECK(emitters == 5);
    CHECK(light_passes(B_AIR) && light_passes(B_GLASS) && light_passes(B_WATER) && light_passes(B_TORCH));
    CHECK(!light_passes(B_STONE) && !light_passes(B_LEAVES));
    /* Flames and lava are warm: red strongest, blue weakest. */
    const uint8_t warm[] = {B_CAMPFIRE, B_LAVA, B_TORCH, B_LAMP};
    for (size_t i = 0; i < sizeof warm; i++) {
        uint16_t e = light_emission(warm[i]);
        CHECK(LIGHT_R(e) >= 14 && LIGHT_R(e) >= LIGHT_G(e) && LIGHT_G(e) >= LIGHT_B(e));
    }
    /* The lamp burns whiter than a torch, a torch whiter than a campfire. */
    CHECK(LIGHT_B(light_emission(B_LAMP)) > LIGHT_B(light_emission(B_TORCH)));
    CHECK(LIGHT_B(light_emission(B_TORCH)) > LIGHT_B(light_emission(B_CAMPFIRE)));
    /* Foxfire is a faint green. */
    uint16_t f = light_emission(B_FOXFIRE);
    CHECK(LIGHT_G(f) > LIGHT_R(f) && LIGHT_G(f) > LIGHT_B(f) && LIGHT_G(f) < 10);
    /* The glow: nothing below the Draper point, a dull red just past it,
     * white when very hot. */
    CHECK(light_glow(-273.0f) == 0 && light_glow(499.0f) == 0);
    CHECK(light_glow(500.0f) == LIGHT_PACK(0, 1, 0, 0));
    CHECK(light_glow(5000.0f) == LIGHT_PACK(0, 15, 15, 15));
    CHECK(light_emission(B_LAVA) == light_glow(1100.0f));
}

/* Coloured lights mix per channel, the brighter one winning; neither
 * adds to the other. */
static void test_mixing(void)
{
    uint8_t *b = mem_calloc((size_t)R * R * WORLD_H, 1); /* all air */
    uint16_t *foot = mem_alloc(FOOT_VOL * sizeof *foot);
    light_emitter red = {CHUNK_W + 2, 40, CHUNK_W + 8, LIGHT_PACK(0, 15, 0, 0)};
    light_emitter blue = {CHUNK_W + 12, 40, CHUNK_W + 8, LIGHT_PACK(0, 0, 0, 15)};
    light_emitter both[2] = {red, blue};
    light_region(b, 64, both, 2, foot);
    uint16_t m = foot_at(foot, 7, 40, 8);
    CHECK(LIGHT_R(m) == 10 && LIGHT_G(m) == 0 && LIGHT_B(m) == 10 && LIGHT_SKY(m) == 15);
    CHECK(LIGHT_R(foot_at(foot, 2, 40, 8)) == 15 && LIGHT_B(foot_at(foot, 2, 40, 8)) == 5);
    /* Two reds four blocks apart: 13 between them, not more. */
    light_emitter reds[2] = {red, red};
    reds[1].x = CHUNK_W + 6;
    light_region(b, 64, reds, 2, foot);
    CHECK(LIGHT_R(foot_at(foot, 4, 40, 8)) == 13);
    /* A dimmer source on a brighter one changes nothing. */
    light_emitter dim[2] = {red, red};
    dim[1].light = LIGHT_PACK(0, 4, 0, 0);
    light_region(b, 64, dim, 2, foot);
    CHECK(LIGHT_R(foot_at(foot, 2, 40, 8)) == 15 && LIGHT_R(foot_at(foot, 5, 40, 8)) == 12);
    /* Sources outside the snapshot or above its top are ignored. */
    light_emitter out[5] = {red, red, red, red, red};
    out[0].x = -1;
    out[1].x = R;
    out[2].z = R;
    out[3].y = -1;
    out[4].y = 64;
    light_region(b, 64, out, 5, foot);
    int lit = 0;
    for (size_t i = 0; i < FOOT_VOL; i++) lit += (foot[i] & 0xFFF0u) != 0;
    CHECK(lit == 0);
    mem_free(foot);
    mem_free(b);
}

/* A hot cell inside rock lights the open cells beside it and nothing
 * through the rock. */
static void test_hot_rock(void)
{
    uint8_t *b = mem_alloc((size_t)R * R * WORLD_H);
    memset(b, B_STONE, (size_t)R * R * WORLD_H);
    b[mid(6, 30, 5)] = B_AIR;
    b[mid(5, 30, 8)] = B_AIR;
    light_emitter hot = {CHUNK_W + 5, 30, CHUNK_W + 5, LIGHT_PACK(0, 12, 4, 0)};
    uint16_t *foot = mem_alloc(FOOT_VOL * sizeof *foot);
    light_region(b, 64, &hot, 1, foot);
    CHECK(foot_at(foot, 5, 30, 5) == LIGHT_PACK(0, 12, 4, 0));
    CHECK(foot_at(foot, 6, 30, 5) == LIGHT_PACK(0, 11, 3, 0));
    CHECK(foot_at(foot, 5, 30, 8) == 0);
    CHECK(foot_at(foot, 5, 31, 5) == 0); /* rock does not carry it on */
    mem_free(foot);
    mem_free(b);
}

/* Glass and water let the sky straight down; the snapshot's top is
 * clamped to the world. */
static void test_sky_through(void)
{
    uint8_t *b = flat_region();
    uint16_t *foot = mem_alloc(FOOT_VOL * sizeof *foot), *f2 = mem_alloc(FOOT_VOL * sizeof *f2);
    for (int z = -16; z < 32; z++)
        for (int x = -16; x < 32; x++) {
            b[mid(x, 24, z)] = B_GLASS;
            b[mid(x, 23, z)] = B_WATER;
        }
    light_region(b, 25, NULL, 0, foot);
    CHECK(foot_at(foot, 5, 20, 5) == LIGHT_OPEN_SKY && foot_at(foot, 5, 23, 5) == LIGHT_OPEN_SKY);
    /* Nothing below the top: all open sky. */
    light_region(b, 0, NULL, 0, foot);
    light_region(b, -5, NULL, 0, f2);
    int sky = 1;
    for (size_t i = 0; i < FOOT_VOL; i++) sky &= foot[i] == LIGHT_OPEN_SKY && f2[i] == LIGHT_OPEN_SKY;
    CHECK(sky);
    mem_free(b);
    b = flat_region();
    light_region(b, WORLD_H, NULL, 0, foot);
    light_region(b, WORLD_H + 50, NULL, 0, f2);
    CHECK(memcmp(foot, f2, FOOT_VOL * sizeof *foot) == 0);
    mem_free(f2);
    mem_free(foot);
    mem_free(b);
}

/* Texels of sections with no array of their own, and the fix-up of
 * opaque texels. */
static void test_uniform_texels(void)
{
    column_light l = {0};
    for (int sy = 0; sy < SECTIONS; sy++) l.fill[sy] = sy < 2 ? 0 : LIGHT_OPEN_SKY;
    l.fill[1] = LIGHT_PACK(9, 1, 2, 3);
    uint16_t *tex = mem_alloc(LIGHT_TEX_VOL * sizeof *tex);
    uint8_t *air = mem_calloc(LIGHT_TEX_VOL, 1);
    uint16_t u = 0;
    light_copy_texels(&l, 1, tex);
    /* Section 0 below, its own cells and border, section 2 above. */
    CHECK(tex[mesh_pidx(0, -1, 0)] == 0);
    CHECK(tex[mesh_pidx(5, 7, 5)] == LIGHT_PACK(9, 1, 2, 3));
    CHECK(tex[mesh_pidx(-1, 15, 16)] == LIGHT_PACK(9, 1, 2, 3));
    CHECK(tex[mesh_pidx(5, 16, 5)] == LIGHT_OPEN_SKY);
    CHECK(light_fix_texels(air, tex, &u) == 0);
    /* Below the world is dark, above it open sky. */
    light_copy_texels(&l, 0, tex);
    CHECK(tex[mesh_pidx(3, -1, 3)] == 0);
    light_copy_texels(&l, SECTIONS - 1, tex);
    CHECK(light_fix_texels(air, tex, &u) == 1 && u == LIGHT_OPEN_SKY);
    /* An opaque cell with no open neighbour keeps its light; one between
     * two open cells takes the lower level of each. */
    uint8_t *rock = mem_alloc(LIGHT_TEX_VOL);
    memset(rock, B_STONE, LIGHT_TEX_VOL);
    for (int i = 0; i < LIGHT_TEX_VOL; i++) tex[i] = (uint16_t)i;
    CHECK(light_fix_texels(rock, tex, &u) == 0 && tex[mesh_pidx(4, 4, 4)] == (uint16_t)mesh_pidx(4, 4, 4));
    rock[mesh_pidx(3, 4, 4)] = B_AIR;
    rock[mesh_pidx(5, 4, 4)] = B_AIR;
    tex[mesh_pidx(3, 4, 4)] = LIGHT_PACK(15, 0, 6, 2);
    tex[mesh_pidx(5, 4, 4)] = LIGHT_PACK(3, 9, 8, 1);
    light_fix_texels(rock, tex, &u);
    CHECK(tex[mesh_pidx(4, 4, 4)] == LIGHT_PACK(3, 0, 6, 1));
    CHECK(tex[mesh_pidx(3, 4, 4)] == LIGHT_PACK(15, 0, 6, 2)); /* open cells untouched */
    mem_free(rock);
    mem_free(air);
    mem_free(tex);
}

/* Every slot has its own place in the atlas, and the largest atlas still
 * fits the packing. */
static void test_slots(void)
{
    int n[3] = {0};
    int total = light_atlas_layout(4096, 3000, n);
    int bad = 0;
    for (int s = 0; s < total; s++) {
        uint32_t b = light_slot_base(s, n);
        uint32_t x = b & 2047u, y = (b >> 11) & 2047u, z = b >> 22;
        bad += x % LIGHT_TEX || y % LIGHT_TEX || z % LIGHT_TEX;
        bad += (int)(x / LIGHT_TEX + (y / LIGHT_TEX) * (uint32_t)n[0] + (z / LIGHT_TEX) * (uint32_t)(n[0] * n[1])) != s;
    }
    CHECK(bad == 0);
    total = light_atlas_layout(16384, 10000000, n);
    uint32_t last = light_slot_base(total - 1, n);
    CHECK((last & 2047u) + LIGHT_TEX <= LIGHT_ATLAS_XY_MAX + 1u);
    CHECK(((last >> 11) & 2047u) + LIGHT_TEX <= LIGHT_ATLAS_XY_MAX + 1u);
    CHECK((last >> 22) + LIGHT_TEX <= LIGHT_ATLAS_Z_MAX + 1u);
}

/* How bright a cell looks: 80% a level, sky by daylight, the brighter of
 * sky and block. */
static void test_brightness(void)
{
    CHECK(light_brightness(LIGHT_OPEN_SKY, 1.0f) == 1.0f);
    CHECK(fabsf(light_brightness(LIGHT_OPEN_SKY, 0.5f) - 0.5f) < 1e-6f);
    CHECK(fabsf(light_brightness(LIGHT_PACK(14, 0, 0, 0), 1.0f) - 0.8f) < 1e-6f);
    CHECK(fabsf(light_brightness(0, 1.0f) - powf(0.8f, 15.0f)) < 1e-6f);
    /* Green counts as much as red; a lamp is bright at night. */
    CHECK(light_brightness(LIGHT_PACK(0, 0, 15, 0), 0.01f) == 1.0f);
    CHECK(light_brightness(LIGHT_PACK(15, 0, 0, 13), 0.1f) > 0.6f);
    int rising = 1;
    for (unsigned s = 1; s <= 15; s++)
        rising &= light_brightness(LIGHT_PACK(s, 0, 0, 0), 1.0f) > light_brightness(LIGHT_PACK(s - 1, 0, 0, 0), 1.0f);
    CHECK(rising);
}

/* What relights when light changes, and light across a column corner. */
static void test_world_relight(void)
{
    test_world t = {0};
    tw_init(&t);
    settle(&t.w);
    CHECK(world_light(&t.w, 3, -1, 3) == 0 && world_light(&t.w, 3, WORLD_H, 3) == LIGHT_OPEN_SKY);
    CHECK(world_light(&t.w, 100000, 5, 3) == LIGHT_OPEN_SKY); /* not loaded */
    /* Glow near a column's corner relights the columns it can reach and
     * not the far corner, 28 blocks off. */
    world_light_touch(&t.w, 1, 1);
    CHECK(t.w.light_pending);
    CHECK(world_column(&t.w, -1, -1)->light_dirty && world_column(&t.w, 0, 0)->light_dirty);
    CHECK(!world_column(&t.w, 1, 1)->light_dirty);
    settle(&t.w);
    CHECK(!world_column(&t.w, -1, -1)->light_dirty && !world_column(&t.w, 0, 0)->light_dirty);
    /* A torch in a column's corner lights the diagonal neighbour. */
    uint16_t e = light_emission(B_TORCH);
    world_set(&t.w, 15, GROUND, 15, B_TORCH, 0);
    settle(&t.w);
    CHECK(LIGHT_R(world_light(&t.w, 16, GROUND, 16)) == LIGHT_R(e) - 2);
    CHECK(LIGHT_G(world_light(&t.w, 17, GROUND, 17)) == LIGHT_G(e) - 4);
    /* Taking it away puts the dark back. */
    world_set(&t.w, 15, GROUND, 15, B_AIR, 0);
    settle(&t.w);
    CHECK(LIGHT_R(world_light(&t.w, 16, GROUND, 16)) == 0 && LIGHT_R(world_light(&t.w, 15, GROUND, 15)) == 0);
    /* Lamps and foxfire light the world too. */
    world_set(&t.w, 8, GROUND, 8, B_LAMP, 0);
    world_set(&t.w, 3, GROUND, 12, B_FOXFIRE, 0);
    settle(&t.w);
    CHECK(LIGHT_B(world_light(&t.w, 8, GROUND + 1, 8)) == LIGHT_B(light_emission(B_LAMP)) - 1);
    CHECK(LIGHT_G(world_light(&t.w, 3, GROUND + 1, 12)) == LIGHT_G(light_emission(B_FOXFIRE)) - 1);
    CHECK(light_at(&t.w, 8.5, GROUND + 1.5, 8.5, 0.0f) > 0.7f);
    /* Lava glows on its own, west of the origin as well (negative
     * coordinates fall in column -1). */
    world_set(&t.w, -6, GROUND, 4, B_LAVA, 0);
    settle(&t.w);
    uint16_t lava = light_emission(B_LAVA);
    CHECK(LIGHT_R(world_light(&t.w, -6, GROUND, 4)) == LIGHT_R(lava));
    CHECK(LIGHT_R(world_light(&t.w, -6, GROUND + 2, 4)) == LIGHT_R(lava) - 2);
    CHECK(LIGHT_G(world_light(&t.w, -3, GROUND, 4)) == LIGHT_G(lava) - 3);
    CHECK(LIGHT_R(world_light(&t.w, 2, GROUND, 4)) == LIGHT_R(lava) - 8);
    /* Under the sky by day the lava's cell is as bright as the open air. */
    CHECK(light_at(&t.w, -5.5, GROUND + 0.5, 4.5, 1.0f) == 1.0f);
    /* At night: -5.01 is in the lava's cell (-6), not truncated to -5. */
    CHECK(light_at(&t.w, -5.01, GROUND + 0.5, 4.5, 0.0f) == light_brightness(lava, 0.0f));
    CHECK(light_at(&t.w, -4.99, GROUND + 0.5, 4.5, 0.0f) < light_brightness(lava, 0.0f));
    world_set(&t.w, -6, GROUND, 4, B_AIR, 0);
    settle(&t.w);
    CHECK(LIGHT_R(world_light(&t.w, -6, GROUND, 4)) == 0 && LIGHT_R(world_light(&t.w, -3, GROUND, 4)) == 0);
    tw_free(&t);
}

/* The recipe that makes out from ingredient, or -1. */
static int find_recipe(uint32_t out, uint32_t ingredient)
{
    for (int i = 0; i < g_recipe_count; i++)
        for (int k = 0; k < 3; k++)
            if (g_recipes[i].out.id == out && g_recipes[i].in[k].id == ingredient) return i;
    return -1;
}

/* Torches and lamps are made by hand, they can be placed, and their
 * textures show a flame. */
static void test_light_items(void)
{
    int coal = find_recipe(B_TORCH, I_COAL), charcoal = find_recipe(B_TORCH, I_CHARCOAL);
    int lamp = find_recipe(B_LAMP, I_OIL_BUCKET);
    CHECK(coal >= 0 && charcoal >= 0 && lamp >= 0 && coal != charcoal);
    if (coal < 0 || charcoal < 0 || lamp < 0) return;
    CHECK(g_recipes[coal].station == 0 && g_recipes[lamp].station == 0);
    inventory inv = {0};
    inv_add(&inv, I_STICK, 2);
    inv_add(&inv, I_COAL, 1);
    CHECK(inv_can_craft(&inv, coal, 0) && !inv_can_craft(&inv, charcoal, 0));
    CHECK(inv_craft(&inv, coal, 0) == 1);
    CHECK(inv_count(&inv, B_TORCH) == 4 && inv_count(&inv, I_STICK) == 1 && inv_count(&inv, I_COAL) == 0);
    CHECK(!inv_can_craft(&inv, coal, 0));
    inv_add(&inv, I_CHARCOAL, 1);
    CHECK(inv_craft(&inv, charcoal, 0) == 1 && inv_count(&inv, B_TORCH) == 8);
    inv_add(&inv, B_GLASS, 1);
    inv_add(&inv, I_COPPER_INGOT, 1);
    CHECK(!inv_can_craft(&inv, lamp, 0)); /* no oil yet */
    inv_add(&inv, I_OIL_BUCKET, 1);
    CHECK(inv_craft(&inv, lamp, 0) == 1);
    CHECK(inv_count(&inv, B_LAMP) == 2 && inv_count(&inv, B_GLASS) == 0 && inv_count(&inv, I_COPPER_INGOT) == 0);
    /* Each places its block and says what it is for. */
    const uint8_t lights[] = {B_TORCH, B_LAMP, B_FOXFIRE};
    for (size_t i = 0; i < sizeof lights; i++) {
        const item_def *d = item_get(lights[i]);
        CHECK(item_valid(lights[i]) && d->block == lights[i] && d->desc && d->desc[0]);
    }
    /* The lamp: a copper frame round a bright flame. */
    uint8_t edge[4] = {0}, flame[4] = {0}, px[4] = {0};
    texgen_texel(T_LAMP, 0, 0, edge);
    texgen_texel(T_LAMP, 7, 8, flame);
    CHECK(edge[0] > edge[2] && edge[3] == 255);
    CHECK(flame[0] + flame[1] + flame[2] > edge[0] + edge[1] + edge[2] + 200);
    /* Foxfire: glowing threads through dark matter. */
    int glow = 0, dark = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            texgen_texel(T_FOXFIRE, x, y, px);
            glow += px[1] > 150 && px[1] > px[0];
            dark += px[1] < 80;
        }
    CHECK(glow > 20 && dark > 20 && glow + dark == 256);
    /* The torch item: clear round the stick, a white-hot heart in the flame. */
    texgen_texel(T_ITEM_TORCH, 0, 0, px);
    CHECK(px[3] == 0);
    texgen_texel(T_ITEM_TORCH, 10, 3, px);
    CHECK(px[3] == 255 && px[0] > 230 && px[1] > 220);
}

void test_light_all(void)
{
    test_atlas();
    test_world_light();
    test_sky();
    test_emitters();
    test_borders();
    test_store();
    test_texels();
    test_curves();
    test_blocks();
    test_mixing();
    test_hot_rock();
    test_sky_through();
    test_uniform_texels();
    test_slots();
    test_brightness();
    test_world_relight();
    test_light_items();
}
