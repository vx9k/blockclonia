/* Unit tests for the engine core (no GPU, no window). */
#include "camera.h"
#include "debug.h"
#include "fx.h"
#include "gpupool.h"
#include "health.h"
#include "hud.h"
#include "interact.h"
#include "inventory.h"
#include "jobs.h"
#include "mem.h"
#include "menu.h"
#include "mesher.h"
#include "physics.h"
#include "save.h"
#include "settings.h"
#include "survival.h"
#include "test_util.h"
#include "ui.h"
#include "world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --------------------------------------------------------------- tests */

static void test_gpupool(void)
{
    gpupool p;
    gpupool_init(&p, 1024, 64);
    uint32_t got;
    uint32_t a = gpupool_alloc(&p, 10, &got);
    CHECK(a == 0 && got == 64);
    uint32_t b = gpupool_alloc(&p, 100, &got);
    CHECK(b == 64 && got == 128);
    uint32_t c = gpupool_alloc(&p, 64, &got);
    CHECK(c == 192);
    gpupool_free(&p, b, 128);
    CHECK(gpupool_alloc(&p, 2000, NULL) == GPUPOOL_FAIL);
    uint32_t d = gpupool_alloc(&p, 128, &got);
    CHECK(d == 64); /* first fit reuses the hole */
    gpupool_free(&p, a, 64);
    gpupool_free(&p, d, 128);
    gpupool_free(&p, c, 64);
    CHECK(p.count == 1 && p.free[0].len == 1024 && p.used == 0); /* fully coalesced */
    gpupool_destroy(&p);
}

static void test_save_roundtrip(void)
{
    column *c = column_alloc(-3, 7);
    test_flat_gen(0, c);
    c->blocks[col_index(3, 20, 4)] = B_WATER;
    c->meta = mem_calloc(COL_VOL, 1);
    c->meta[col_index(3, 20, 4)] = 5;
    uint8_t *buf = mem_alloc(SAVE_MAX_FILE);
    size_t len = save_encode_column(c, buf, SAVE_MAX_FILE);
    CHECK(len > 24 && len < 4096); /* flat terrain compresses well */

    column *d = column_alloc(-3, 7);
    CHECK(save_decode_column(d, buf, len) == 0);
    CHECK(memcmp(c->blocks, d->blocks, COL_VOL) == 0);
    CHECK(d->meta && d->meta[col_index(3, 20, 4)] == 5);

    /* Corruption is always rejected. */
    buf[len - 1] ^= 0x55;
    CHECK(save_decode_column(d, buf, len) == -1);
    buf[len - 1] ^= 0x55;
    CHECK(save_decode_column(d, buf, len - 1) == -1);
    column *wrong = column_alloc(0, 0);
    CHECK(save_decode_column(wrong, buf, len) == -1);
    CHECK(save_decode_column(d, buf, 10) == -1);

    column_free(c);
    column_free(d);
    column_free(wrong);
    mem_free(buf);
}

static void fill_input(mesh_input *in, uint8_t fill)
{
    memset(in->blocks, fill, sizeof in->blocks);
    memset(in->meta, 0, sizeof in->meta);
}

static int file_is(const char *path, const char *text)
{
    char buf[64] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    (void)fclose(f);
    return n == strlen(text) && memcmp(buf, text, n) == 0;
}

/* A shared world folder must not be able to redirect saves elsewhere. */
static void test_save_files(void)
{
    char dir[256];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/mc_test_XXXXXX", tmp && tmp[0] && strlen(tmp) < 200 ? tmp : "/tmp");
    int made = mkdtemp(dir) != NULL;
    CHECK(made);
    if (!made) return;
    char victim[300], p[340];
    snprintf(victim, sizeof victim, "%s/victim.txt", dir);
    FILE *f = fopen(victim, "wb");
    CHECK(f && fputs("precious", f) >= 0);
    if (f) (void)fclose(f);

    snprintf(p, sizeof p, "%s/level.dat", dir);
    CHECK(symlink(victim, p) == 0);
    uint32_t seed = 0;
    CHECK(save_read_seed(dir, &seed) == -1); /* symlinks are not followed */
    CHECK(save_write_seed(dir, 77) == 0);
    CHECK(file_is(victim, "precious"));
    CHECK(save_read_seed(dir, &seed) == 0 && seed == 77);
    struct stat st;
    CHECK(lstat(p, &st) == 0 && S_ISREG(st.st_mode)); /* the link was replaced */

    column *c = column_alloc(0, 0);
    test_flat_gen(0, c);
    snprintf(p, sizeof p, "%s/c.0.0.bin.tmp", dir);
    CHECK(symlink(victim, p) == 0);
    CHECK(save_store_column(dir, c) == 0);
    CHECK(file_is(victim, "precious"));
    column *d = column_alloc(0, 0);
    CHECK(save_load_column(dir, d) == 1 && memcmp(c->blocks, d->blocks, COL_VOL) == 0);

    /* A FIFO in place of a column file is rejected, not waited on. */
    snprintf(p, sizeof p, "%s/c.1.0.bin", dir);
    CHECK(mkfifo(p, 0600) == 0);
    column *e = column_alloc(1, 0);
    CHECK(save_load_column(dir, e) == -1);
    column *g = column_alloc(2, 0);
    CHECK(save_load_column(dir, g) == 0); /* missing: generate */

    column_free(c);
    column_free(d);
    column_free(e);
    column_free(g);
    const char *names[] = {"victim.txt", "level.dat", "c.0.0.bin", "c.1.0.bin"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        unlink(p);
    }
    rmdir(dir);
}

static void test_mesher(void)
{
    mesh_input *in = mem_alloc(sizeof *in);
    uint32_t *out = mem_alloc(sizeof(uint32_t) * 4 * MESH_MAX_QUADS);
    uint32_t o, t;

    fill_input(in, B_AIR);
    in->blocks[mesh_pidx(5, 5, 5)] = B_STONE;
    CHECK(mesh_section(in, out, &o, &t) == 6 && o == 6 && t == 0);

    /* Greedy merge: a 4x1x1 bar is still 6 quads. */
    for (int x = 5; x < 9; x++) in->blocks[mesh_pidx(x, 5, 5)] = B_STONE;
    CHECK(mesh_section(in, out, &o, &t) == 6);

    /* Fully buried section has nothing to draw. */
    fill_input(in, B_STONE);
    CHECK(mesh_section(in, out, &o, &t) == 0);

    /* A full 16^3 cube of glass in air: 6 merged faces, all translucent. */
    fill_input(in, B_AIR);
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) in->blocks[mesh_pidx(x, y, z)] = B_GLASS;
    CHECK(mesh_section(in, out, &o, &t) == 6 && o == 0 && t == 6);

    /* Checkerboard worst case stays within MESH_MAX_QUADS. */
    fill_input(in, B_AIR);
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++)
                if ((x + y + z) & 1) in->blocks[mesh_pidx(x, y, z)] = B_STONE;
    CHECK(mesh_section(in, out, &o, &t) == 2048 * 6);

    /* Partial water gets a lowered surface. */
    fill_input(in, B_AIR);
    in->blocks[mesh_pidx(2, 2, 2)] = B_WATER;
    in->meta[mesh_pidx(2, 2, 2)] = 4;
    mesh_section(in, out, &o, &t);
    CHECK(t == 6);
    int found_drop = 0;
    for (uint32_t i = 0; i < t * 4; i++) found_drop |= (out[i] >> 28) == 4;
    CHECK(found_drop);

    mem_free(in);
    mem_free(out);
}

static void test_world_basics(void)
{
    test_world t;
    tw_init(&t);
    CHECK(world_get(&t.w, 0, 0, 0) == B_BEDROCK);
    CHECK(world_get(&t.w, -5, 12, -9) == B_GRASS);
    CHECK(world_get(&t.w, 3, 13, 3) == B_AIR);
    CHECK(world_get(&t.w, 1000, 20, 0) == B_UNLOADED);
    CHECK(world_surface_y(&t.w, 4, 4) == GROUND);
    CHECK(world_set(&t.w, 3, 13, 3, B_BRICK, 0));
    CHECK(world_get(&t.w, 3, 13, 3) == B_BRICK);
    const column *c = world_column(&t.w, 0, 0);
    CHECK(c && c->modified && c->unsaved && c->dirty[0]);
    /* Edits on a column border dirty the neighbour's section too. */
    column *n = world_column(&t.w, -1, 0);
    n->dirty[0] = 0;
    world_set(&t.w, 0, 13, 5, B_BRICK, 0);
    CHECK(n->dirty[0]);
    tw_free(&t);
}

static void test_player_physics(void)
{
    test_world t;
    tw_init(&t);
    player p;
    player_spawn(&p, &t.w, 0.5, 0.5);
    CHECK(fabs(p.pos.y - GROUND) < 1e-9);

    /* Free fall from 20 m: lands on the ground, doesn't tunnel, and takes
     * about sqrt(2h/g) = 2.02 s (air drag adds a little). */
    p.pos.y = GROUND + 20;
    player_input idle = {0};
    int steps = 0;
    while (!p.on_ground && steps < 600) {
        physics_step(&t.ph, &p, &idle);
        steps++;
    }
    double tfall = steps * PHYS_DT;
    CHECK(p.on_ground);
    CHECK(fabs(p.pos.y - GROUND) < 1e-6);
    CHECK(tfall > 1.95 && tfall < 2.15);

    /* A very fast fall still stops on a 1-block platform. (It must be a
     * pillar: a lone floating block would collapse.) */
    for (int y = GROUND; y <= 40; y++) world_set(&t.w, 0, y, 0, B_STONE, 0);
    p.pos = dv3(0.5, 90.0, 0.5);
    p.vel = dv3(0, -60, 0);
    run_steps(&t, &p, &idle, 300);
    CHECK(fabs(p.pos.y - 41.0) < 1e-6);

    /* Walls stop horizontal motion. */
    player_spawn(&p, &t.w, 8.5, 8.5);
    for (int y = GROUND; y < GROUND + 3; y++) world_set(&t.w, 8, y, 4, B_STONE, 0);
    player_input fwd = {.forward = 1.0f}; /* yaw 0 faces -Z */
    run_steps(&t, &p, &fwd, 240);
    CHECK(fabs(p.pos.z - (5.0 + PLAYER_HALF_W)) < 1e-6);

    /* Jumping clears exactly one block. */
    player_spawn(&p, &t.w, 5.5, 5.5);
    player_input jump = {.jump = 1};
    double peak = p.pos.y;
    for (int i = 0; i < 90; i++) {
        jump.jump = i < 5; /* hold briefly: the first step only finds the ground */
        physics_step(&t.ph, &p, &jump);
        if (p.pos.y > peak) peak = p.pos.y;
    }
    CHECK(peak - GROUND > 1.0 && peak - GROUND < 1.3);
    tw_free(&t);
}

static void test_sprint_sneak(void)
{
    test_world t;
    tw_init(&t);
    player p;

    /* Sprinting forward reaches sprint speed; walking stays at walking pace. */
    player_spawn(&p, &t.w, 0.5, 20.5);
    player_input walk = {.forward = 1.0f};
    run_steps(&t, &p, &walk, 120);
    CHECK(!p.sprinting && fabs(hypot(p.vel.x, p.vel.z) - WALK_SPEED) < 0.05);
    player_input run = {.forward = 1.0f, .sprint = 1};
    run_steps(&t, &p, &run, 120);
    CHECK(p.sprinting && fabs(hypot(p.vel.x, p.vel.z) - SPRINT_SPEED) < 0.05);
    /* Letting go of forward, or sneaking, ends the sprint. */
    player_input side = {.right = 1.0f, .sprint = 1};
    physics_step(&t.ph, &p, &side);
    CHECK(!p.sprinting);
    player_input sneak_run = {.forward = 1.0f, .sprint = 1, .sneak = 1};
    run_steps(&t, &p, &sneak_run, 120);
    CHECK(!p.sprinting && p.sneaking && fabs(hypot(p.vel.x, p.vel.z) - SNEAK_SPEED) < 0.05);

    /* A sprint does not start in mid-air, but carries through a jump. */
    player_spawn(&p, &t.w, 0.5, 20.5);
    p.pos.y += 3.0;
    physics_step(&t.ph, &p, &run);
    CHECK(!p.sprinting);
    player_spawn(&p, &t.w, 0.5, 20.5);
    run_steps(&t, &p, &run, 30);
    player_input run_jump = run;
    run_jump.jump = 1;
    physics_step(&t.ph, &p, &run_jump);
    physics_step(&t.ph, &p, &run);
    CHECK(!p.on_ground && p.sprinting);

    /* Running into a wall stops the sprint. */
    player_spawn(&p, &t.w, 8.5, 8.5);
    for (int y = GROUND; y < GROUND + 3; y++) world_set(&t.w, 8, y, 5, B_STONE, 0);
    run_steps(&t, &p, &run, 120);
    CHECK(!p.sprinting && p.hit_wall);

    /* Sneaking off a ledge: the player stops at the edge instead. The pad
     * is two blocks up, so the edge is a real drop. */
    for (int z = -8; z <= -4; z++)
        for (int y = GROUND; y < GROUND + 2; y++) world_set(&t.w, -8, y, z, B_STONE, 0);
    player_spawn(&p, &t.w, -7.5, -6.5);
    CHECK(fabs(p.pos.y - (GROUND + 2)) < 1e-9);
    player_input edge = {.forward = 1.0f, .sneak = 1};
    run_steps(&t, &p, &edge, 300);
    CHECK(fabs(p.pos.y - (GROUND + 2)) < 1e-6);        /* still on the pad */
    CHECK(p.pos.z < -8.0 + 0.05 && p.pos.z > -8.0 - PLAYER_HALF_W - 0.05); /* leaning over the edge */
    player_input off = {.forward = 1.0f};
    run_steps(&t, &p, &off, 120);
    CHECK(p.pos.y < GROUND + 0.5 && p.fall_speed == 0.0); /* walked off without the guard */
    tw_free(&t);
}

static void test_camera(void)
{
    camera_anim c;
    camera_init(&c);
    player p;
    memset(&p, 0, sizeof p);
    p.on_ground = 1;
    p.vel = dv3(0, 0, -WALK_SPEED);
    /* Walking: the head bobs, and more than standing still (zero). */
    float lo = 0.0f, hi = -1.0f;
    for (int i = 0; i < 240; i++) {
        camera_pose o = camera_update(&c, &p, 0.0f, 0.0f, 1, 1, 1.0f / 60.0f);
        if (i > 60) {
            lo = fminf(lo, o.dy);
            hi = fmaxf(hi, o.dy);
        }
        CHECK(o.fov_scale == 1.0f || i < 1);
    }
    CHECK(lo < -0.02f && hi > -0.005f);
    /* Bobbing off: no offset at all. */
    camera_pose off = camera_update(&c, &p, 0.0f, 0.0f, 0, 1, 1.0f / 60.0f);
    CHECK(off.dx == 0.0f && off.dy == 0.0f && off.roll == 0.0f);
    /* Sprinting widens the view by up to 12%. */
    p.sprinting = 1;
    p.vel = dv3(0, 0, -SPRINT_SPEED);
    camera_pose o = {0};
    for (int i = 0; i < 120; i++) o = camera_update(&c, &p, 0.0f, 0.0f, 1, 1, 1.0f / 60.0f);
    CHECK(o.fov_scale > 1.11f && o.fov_scale <= 1.12f);
    o = camera_update(&c, &p, 0.0f, 0.0f, 1, 0, 1.0f / 60.0f);
    CHECK(o.fov_scale == 1.0f);
    /* A hard landing dips the head, which comes back within half a second. */
    camera_init(&c);
    memset(&p, 0, sizeof p);
    p.on_ground = 1;
    float deepest = 0.0f;
    for (int i = 0; i < 60; i++) {
        o = camera_update(&c, &p, i == 0 ? 10.0f : 0.0f, 0.0f, 1, 1, 1.0f / 60.0f);
        deepest = fminf(deepest, o.dy);
    }
    CHECK(deepest < -0.1f && fabsf(o.dy) < 0.02f);
    /* Sneaking lowers the eyes even with bobbing off. */
    p.sneaking = 1;
    for (int i = 0; i < 60; i++) o = camera_update(&c, &p, 0.0f, 0.0f, 0, 0, 1.0f / 60.0f);
    CHECK(fabsf(o.dy + 0.3f) < 0.01f);
}

static double slide_distance(uint8_t floor_block)
{
    test_world t;
    tw_init(&t);
    for (int z = -30; z <= 30; z++)
        for (int x = -2; x <= 2; x++) world_set(&t.w, x, GROUND - 1, z, floor_block, 0);
    player p;
    player_spawn(&p, &t.w, 0.5, 20.5);
    p.vel = dv3(0, 0, -4.0);
    player_input idle = {0};
    double z0 = p.pos.z;
    run_steps(&t, &p, &idle, 600);
    double d = z0 - p.pos.z;
    tw_free(&t);
    return d;
}

static void test_friction(void)
{
    double stone = slide_distance(B_STONE), ice = slide_distance(B_ICE);
    /* v^2 / (2 mu g): stone ~1.2 m, ice ~27 m (capped by the 30 m strip). */
    CHECK(stone > 0.8 && stone < 1.6);
    CHECK(ice > 10.0 * stone);
}

static void test_structure(void)
{
    test_world t;
    tw_init(&t);
    physics *ph = &t.ph;

    /* Stone cantilever: 7 blocks out from a pillar holds, the 8th breaks. */
    int x0 = 0, z0 = 0, top = GROUND + 5;
    for (int y = GROUND; y <= top; y++) world_set(&t.w, x0, y, z0, B_STONE, 0);
    for (int i = 1; i <= 7; i++) world_set(&t.w, x0 + i, top, z0, B_STONE, 0);
    CHECK(physics_check_structure(ph, x0 + 7, top, z0) == 0);
    world_set(&t.w, x0 + 8, top, z0, B_STONE, 0);
    CHECK(physics_check_structure(ph, x0 + 8, top, z0) == 1);
    CHECK(world_get(&t.w, x0 + 8, top, z0) == B_AIR);
    CHECK(ph->body_count == 1);

    /* Knock out the pillar: the whole arm comes down. */
    ph->body_count = 0;
    world_set(&t.w, x0, GROUND + 2, z0, B_AIR, 0);
    int fell = physics_check_structure(ph, x0, GROUND + 2, z0);
    CHECK(fell == 3 + 7); /* pillar above the gap plus the arm */

    /* The bodies fall, land and re-solidify. */
    player p;
    player_spawn(&p, &t.w, 10.5, 10.5);
    player_input idle = {0};
    run_steps(&t, &p, &idle, 240);
    CHECK(ph->body_count == 0);
    CHECK(world_get(&t.w, x0 + 3, GROUND, z0) == B_STONE);

    /* Sand stuck to a wall falls; sand on the ground stays. */
    world_set(&t.w, 5, GROUND, 5, B_STONE, 0);
    world_set(&t.w, 5, GROUND + 1, 5, B_STONE, 0);
    world_set(&t.w, 6, GROUND + 1, 5, B_SAND, 0);
    CHECK(physics_check_structure(ph, 6, GROUND + 1, 5) == 1);
    ph->body_count = 0;

    /* Timber: cut a tree's trunk and the tree comes down. */
    int tx = -8, tz = -8;
    for (int y = GROUND; y < GROUND + 5; y++) world_set(&t.w, tx, y, tz, B_LOG, 0);
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dz) world_set(&t.w, tx + dx, GROUND + 4, tz + dz, B_LEAVES, 0);
    ph->struct_count = 0;
    world_set(&t.w, tx, GROUND, tz, B_AIR, 0);
    CHECK(physics_check_structure(ph, tx, GROUND, tz) == 4 + 8);
    tw_free(&t);
}

static void test_buoyancy(void)
{
    test_world t;
    tw_init(&t);
    /* A 6-deep pool. */
    for (int y = GROUND - 6; y < GROUND; y++)
        for (int z = -3; z <= 3; z++)
            for (int x = -3; x <= 3; x++) world_set(&t.w, x, y, z, B_WATER, 0);
    t.ph.fluid_next.count = 0; /* level pool: nothing to simulate */
    memset(t.ph.fluid_next.set, 0, t.ph.fluid_next.set_cap * sizeof(uint32_t));

    t.ph.bodies[0] = (body){.pos = dv3(0, GROUND - 5, 0), .prev_pos = dv3(0, GROUND - 5, 0), .block = B_LOG};
    t.ph.bodies[1] = (body){.pos = dv3(2, GROUND - 1, 2), .prev_pos = dv3(2, GROUND - 1, 2), .block = B_STONE};
    t.ph.body_count = 2;
    player p;
    player_spawn(&p, &t.w, 20.5, 20.5);
    player_input idle = {0};
    double max_log_y = 0;
    for (int i = 0; i < 400 && t.ph.body_count; i++) {
        physics_step(&t.ph, &p, &idle);
        for (int b = 0; b < t.ph.body_count; b++)
            if (t.ph.bodies[b].block == B_LOG && t.ph.bodies[b].pos.y > max_log_y) max_log_y = t.ph.bodies[b].pos.y;
    }
    /* The log (600 kg/m^3) rose towards the surface; the stone sank to the
     * pool floor and set there. */
    CHECK(max_log_y > GROUND - 2.0);
    CHECK(world_get(&t.w, 2, GROUND - 6, 2) == B_STONE);
    tw_free(&t);
}

static void test_settle(void)
{
    test_world t;
    tw_init(&t);
    /* Falling bodies are put back into the world before a save. */
    t.ph.bodies[0] = (body){.pos = dv3(2, GROUND + 6.3, 2), .block = B_SAND};
    t.ph.bodies[1] = (body){.pos = dv3(40, GROUND + 3, 2), .block = B_STONE}; /* another column */
    t.ph.body_count = 2;
    physics_settle_bodies(&t.ph, world_column(&t.w, 0, 0));
    CHECK(t.ph.body_count == 1 && t.ph.bodies[0].block == B_STONE);
    CHECK(world_get(&t.w, 2, GROUND, 2) == B_SAND); /* dropped onto the ground */
    physics_settle_bodies(&t.ph, NULL);
    CHECK(t.ph.body_count == 0 && world_get(&t.w, 40, GROUND, 2) == B_STONE);
    tw_free(&t);
}

static long total_water(const world *w, int r)
{
    long v = 0;
    for (int y = 1; y < 40; y++)
        for (int z = -r; z <= r; z++)
            for (int x = -r; x <= r; x++) v += world_water_level(w, x, y, z);
    return v;
}

static void test_fluid(void)
{
    test_world t;
    tw_init(&t);
    /* Pour 5 full blocks of water from above onto flat ground. */
    for (int y = GROUND + 3; y < GROUND + 8; y++) world_set(&t.w, 0, y, 0, B_WATER, 0);
    long before = total_water(&t.w, 20);
    CHECK(before == 5 * WATER_FULL);
    int ticks = 0;
    while (t.ph.fluid_next.count && ticks < 500) {
        physics_fluid_tick(&t.ph);
        ticks++;
    }
    CHECK(ticks < 500);                       /* it settles */
    CHECK(total_water(&t.w, 20) == before);   /* and conserves volume */
    CHECK(world_get(&t.w, 0, GROUND + 3, 0) == B_AIR); /* nothing left hanging */
    CHECK(world_water_level(&t.w, 0, GROUND, 0) > 0);
    tw_free(&t);
}

static void test_raycast(void)
{
    test_world t;
    tw_init(&t);
    ray_hit h = physics_raycast(&t.w, dv3(0.5, GROUND + 1.6, 0.5), v3(0, -1, 0), 5.0);
    CHECK(h.hit && h.block.y == GROUND - 1 && h.before.y == GROUND && h.id == B_GRASS);
    h = physics_raycast(&t.w, dv3(0.5, GROUND + 1.6, 0.5), v3(0, 1, 0), 5.0);
    CHECK(!h.hit);
    tw_free(&t);
}

/* ---------------------------------------------------------------- health */

static float wave_span(const float *w)
{
    float lo = w[0], hi = w[0];
    for (int i = 1; i < HEALTH_WAVE_LEN; i++) {
        if (w[i] < lo) lo = w[i];
        if (w[i] > hi) hi = w[i];
    }
    return hi - lo;
}

/* A healthy adult at rest reads like one, and the monitor traces move. */
static void test_health_rest(void)
{
    health *h = new_body(1);
    health_env e = calm_env();
    live(h, &e, 120.0);
    CHECK(!h->dead && h->conscious == CONS_ALERT);
    CHECK(h->hr > 55.0f && h->hr < 90.0f);
    CHECK(h->map > 80.0f && h->map < 105.0f);
    CHECK(h->sbp > h->dbp && h->sbp < 140.0f);
    CHECK(health_spo2_reading(h) >= 95);
    CHECK(h->rr > 10.0f && h->rr < 20.0f);
    CHECK(fabsf(h->temp - 37.0f) < 0.3f);
    CHECK(wave_span(h->ecg) > 0.8f);   /* QRS complexes */
    CHECK(wave_span(h->art) > 25.0f);  /* pulse pressure */
    CHECK(wave_span(h->capno) > 25.0f); /* breaths */
    char buf[96];
    for (int p = 0; p < BP_COUNT; p++) {
        CHECK(health_part_status(h, p, buf, sizeof buf) == 0);
        CHECK(!strcmp(buf, "OK"));
    }
    mem_free(h);
}

/* Sprinting drives the heart and lungs up and spends the anaerobic
 * reserve; rest brings them back. */
static void test_health_exercise(void)
{
    health *h = new_body(2);
    health_env e = calm_env();
    live(h, &e, 30.0);
    float hr0 = h->hr;
    e.speed = 6.5;
    live(h, &e, 60.0);
    CHECK(h->hr > 150.0f && h->sbp > 150.0f && h->rr > 25.0f);
    CHECK(health_stamina(h) < 0.5f);
    CHECK(h->lactate > 4.0f);
    health_limits l = health_get_limits(h);
    CHECK(l.move_scale <= 1.0f);
    e.speed = 0.0;
    live(h, &e, 240.0);
    CHECK(h->hr < hr0 + 30.0f);
    CHECK(health_stamina(h) > 0.6f);
    mem_free(h);
}

/* A cut artery drains blood fast, the heart races to hold pressure, and a
 * pressure dressing applied early stops most of it. */
static void test_health_bleeding(void)
{
    health *a = new_body(3), *b = new_body(3);
    health_env e = calm_env();
    health_cut(a, BP_LLEG, 0.8f, 1, 0.2f);
    health_cut(b, BP_LLEG, 0.8f, 1, 0.2f);
    inventory ia, ib;
    inv_init(&ia);
    inv_init(&ib);
    inv_add(&ib, I_BANDAGE, 1);
    char msg[128];
    CHECK(health_treat(b, &ib, BP_LLEG, TREAT_BANDAGE, 0, msg, sizeof msg) == 1);
    CHECK(inv_count(&ib, I_BANDAGE) == 0);
    live(a, &e, 60.0);
    live(b, &e, 60.0);
    CHECK(a->bleed_ext > 200.0f);
    CHECK(b->bleed_ext < 0.5f * a->bleed_ext);
    CHECK(a->blood < BLOOD_NORMAL - 0.4f);
    CHECK(a->hr > b->hr);
    live(a, &e, 240.0);
    live(b, &e, 240.0);
    CHECK(a->conscious != CONS_ALERT); /* class IV haemorrhage */
    CHECK(health_spo2_reading(a) < 0); /* no pulse at the finger */
    CHECK(b->conscious == CONS_ALERT && b->blood > a->blood + 0.5f);
    /* A second dressing needs supplies: two plant fibres make one. */
    CHECK(health_treat(a, &ia, BP_LLEG, TREAT_BANDAGE, 0, msg, sizeof msg) == 0); /* none left */
    inv_add(&ia, I_FIBRE, 2);
    CHECK(health_treat(a, &ia, BP_LLEG, TREAT_BANDAGE, 0, msg, sizeof msg) == (a->conscious != CONS_UNCONSCIOUS));
    CHECK(health_treat(b, &ib, BP_HEAD, TREAT_BANDAGE, 0, msg, sizeof msg) == 0); /* no wound there */
    mem_free(a);
    mem_free(b);
}

/* Holding breath under water: oxygen falls, then consciousness, then the
 * heart stops. Surfacing in time recovers. */
static void test_health_drowning(void)
{
    health *h = new_body(4);
    health_env e = calm_env();
    e.airway = AIRWAY_WATER;
    e.submerged = 1.0;
    live(h, &e, 45.0);
    CHECK(!h->breathing && h->conscious == CONS_ALERT);
    float sat = h->sao2;
    health *s = new_body(4);
    *s = *h;
    health_env air = calm_env();
    air.submerged = 0.8;
    live(s, &air, 30.0);
    CHECK(!s->dead && s->breathing && s->sao2 > 0.93f && s->lung_water < 0.01f);
    /* Past the breaking point the body gasps and inhales water. */
    live(h, &e, 60.0);
    CHECK(h->sao2 < sat - 0.2f);
    CHECK(h->hr < 70.0f); /* diving reflex */
    CHECK(h->lung_water > 0.05f);
    char buf[64];
    CHECK(health_organ_status(h, ORG_LUNGS, buf, sizeof buf) >= 2);
    live(h, &e, 420.0);
    CHECK(h->dead && h->cause == DEATH_DROWNING);
    mem_free(h);
    mem_free(s);
}

/* Falls: a short drop is harmless, a high one breaks legs, and a broken
 * leg stops sprinting and jumping until it is splinted and healed. */
static void test_health_fractures(void)
{
    health *h = new_body(5);
    inventory inv;
    inv_init(&inv);
    health_env e = calm_env();
    health_fall(h, sqrt(2.0 * 9.81 * 1.2), 1.0); /* jumping off a block */
    live(h, &e, 5.0);
    int broken = 0;
    for (int p = 0; p < BP_COUNT; p++) broken += h->part[p].fracture != FX_NONE;
    CHECK(broken == 0);
    health_limits l = health_get_limits(h);
    CHECK(l.can_jump && l.can_sprint && l.move_scale > 0.99f);

    health_break_bone(h, BP_RLEG, 0);
    live(h, &e, 5.0);
    CHECK(h->part[BP_RLEG].fracture == FX_CLOSED);
    l = health_get_limits(h);
    CHECK(!l.can_sprint && !l.can_jump && l.move_scale < 0.6f);
    char msg[128], buf[96];
    CHECK(health_part_status(h, BP_RLEG, buf, sizeof buf) >= 2);
    CHECK(strstr(buf, "closed fracture") != NULL);
    float pain = h->part[BP_RLEG].pain;
    CHECK(health_treat(h, &inv, BP_RLEG, TREAT_SPLINT, 0, msg, sizeof msg) == 0); /* nothing to splint with */
    inv_add(&inv, I_STICK, 2);
    inv_add(&inv, I_FIBRE, 1);
    CHECK(health_treat(h, &inv, BP_RLEG, TREAT_SPLINT, 0, msg, sizeof msg) == 1);
    CHECK(h->part[BP_RLEG].splinted && inv_count(&inv, I_STICK) == 0 && inv_count(&inv, I_FIBRE) == 0);
    live(h, &e, 5.0);
    CHECK(h->part[BP_RLEG].pain < pain);
    CHECK(health_treat(h, &inv, BP_HEAD, TREAT_SPLINT, 0, msg, sizeof msg) == 0);

    /* 20 m onto stone: at best both legs broken. */
    health *f = new_body(6);
    health_fall(f, sqrt(2.0 * 9.81 * 20.0), 1.0);
    live(f, &e, 10.0);
    CHECK(f->dead || f->part[BP_LLEG].fracture || f->part[BP_RLEG].fracture);
    mem_free(h);
    mem_free(f);
}

/* A dirty wound left alone gets infected, then septic with a fever; the
 * same wound cleaned and treated with antibiotics does not. */
static void test_health_infection(void)
{
    health *a = new_body(7), *b = new_body(7);
    health_env e = calm_env();
    e.speed = 0.0;
    health_cut(a, BP_RARM, 0.6f, 0, 0.5f);
    health_cut(b, BP_RARM, 0.6f, 0, 0.5f);
    char msg[128];
    inventory inv;
    inv_init(&inv);
    inv_add(&inv, I_ANTISEPTIC, 1);
    inv_add(&inv, I_ANTIBIOTIC, 1);
    inv_add(&inv, I_BANDAGE, 1);
    CHECK(health_treat(b, &inv, BP_RARM, TREAT_DISINFECT, 0, msg, sizeof msg) == 1);
    CHECK(health_treat(b, &inv, BP_RARM, TREAT_BANDAGE, 0, msg, sizeof msg) == 1);
    CHECK(health_treat(b, &inv, BP_RARM, TREAT_ANTIBIOTIC, 0, msg, sizeof msg) == 1);
    CHECK(inv_count(&inv, I_ANTISEPTIC) + inv_count(&inv, I_ANTIBIOTIC) + inv_count(&inv, I_BANDAGE) == 0);
    /* 30 minutes of real time is 36 hours on the survival clock. Food and
     * water keep both bodies from starving meanwhile. */
    for (int m = 0; m < 30; m++) {
        a->stomach_water = b->stomach_water = 0.3f;
        a->stomach_kcal = b->stomach_kcal = 300.0f;
        live(a, &e, 60.0);
        live(b, &e, 60.0);
    }
    float inf_a = a->wound_count ? a->wounds[0].infection : 0.0f;
    float inf_b = b->wound_count ? b->wounds[0].infection : 0.0f;
    CHECK(inf_a > 0.6f);
    CHECK(a->sepsis > 0.3f && a->temp > 38.0f && a->hr > 90.0f);
    CHECK(inf_b < 0.2f && b->sepsis < 0.05f);
    char buf[96];
    CHECK(health_part_status(a, BP_RARM, buf, sizeof buf) == 3);
    CHECK(strstr(buf, "infection") != NULL);
    mem_free(a);
    mem_free(b);
}

/* Thirst and hunger build on the survival clock; drinking and eating fix
 * them, and breaking leaves finds food. */
static void test_health_needs(void)
{
    health *h = new_body(8);
    inventory inv;
    inv_init(&inv);
    health_env e = calm_env();
    live(h, &e, 600.0); /* half a survival day */
    float hyd = health_hydration(h), hun = health_hunger(h);
    CHECK(hyd < 0.9f && hun > 0.2f);
    char msg[128];
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_DRINK, 0, msg, sizeof msg) == 0); /* no water in reach */
    for (int i = 0; i < 4; i++) {
        CHECK(health_treat(h, &inv, BP_CHEST, TREAT_DRINK, 1, msg, sizeof msg) == 1);
        live(h, &e, 30.0);
    }
    CHECK(health_hydration(h) > hyd);
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_EAT, 0, msg, sizeof msg) == 0); /* no food */
    uint32_t rng = 99;
    item_stack drops[3];
    CHECK(item_drops(B_LOG, &rng, drops) == 1 && drops[0].id == B_LOG);
    for (int i = 0; i < 60; i++) {
        int n = item_drops(B_LEAVES, &rng, drops);
        for (int k = 0; k < n; k++) inv_add(&inv, drops[k].id, drops[k].count);
    }
    CHECK(inv_count(&inv, I_FIBRE) > 10 && inv_count(&inv, I_APPLE) > 0);
    int apples = inv_count(&inv, I_APPLE);
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_EAT, 0, msg, sizeof msg) == 1);
    CHECK(inv_count(&inv, I_APPLE) == apples - 1);
    mem_free(h);
}

/* The game side: landings from the player physics become injuries, and a
 * broken leg slows the player's input. */
static void test_survival(void)
{
    test_world t;
    tw_init(&t);
    player p;
    player_input idle = {0};
    health *h = new_body(9);

    /* Stepping off one block: no injury. */
    player_spawn(&p, &t.w, 4.5, 4.5);
    p.pos.y = GROUND + 1.0;
    for (int i = 0; i < 120; i++) {
        survival_before b = survival_capture(&p);
        physics_step(&t.ph, &p, &idle);
        survival_impacts(h, &t.w, &t.ph, &p, &b);
    }
    CHECK(p.on_ground && h->wound_count == 0 && !h->part[BP_LLEG].fracture && !h->part[BP_RLEG].fracture);

    /* 12 m onto grass: the legs take it. */
    p.pos.y = GROUND + 12.0;
    p.vel = dv3(0, 0, 0);
    p.on_ground = 0;
    float before = h->part[BP_LLEG].integrity + h->part[BP_RLEG].integrity;
    for (int i = 0; i < 240; i++) {
        survival_before b = survival_capture(&p);
        physics_step(&t.ph, &p, &idle);
        survival_impacts(h, &t.w, &t.ph, &p, &b);
    }
    CHECK(p.on_ground);
    CHECK(h->part[BP_LLEG].integrity + h->part[BP_RLEG].integrity < before - 0.2f);

    /* Environment: air at the eyes on dry land, water when submerged. */
    health_env e;
    survival_before b = survival_capture(&p);
    survival_env(&t.w, &p, &b, 0, &e);
    CHECK(e.airway == AIRWAY_AIR && e.on_ground);
    for (int y = GROUND; y < GROUND + 3; y++) world_set(&t.w, 4, y, 4, B_WATER, 0);
    survival_env(&t.w, &p, &b, 0, &e);
    CHECK(e.airway == AIRWAY_WATER);

    /* Input limits follow the body; flying ignores them. */
    health *k = new_body(10);
    health_break_bone(k, BP_LLEG, 0);
    player_input in = {.forward = 1.0f, .jump = 1, .sprint = 1};
    p.submerged = 0.0;
    survival_limit_input(k, &p, &in);
    CHECK(in.forward < 0.6f && !in.jump && !in.sprint);
    player_input fly = {.forward = 1.0f, .jump = 1, .sprint = 1};
    p.flying = 1;
    survival_limit_input(k, &p, &fly);
    CHECK(fly.forward == 1.0f && fly.jump && fly.sprint);

    /* Breaking glass by hand cuts the arm. */
    health *g = new_body(11);
    for (int i = 0; i < 3; i++) survival_on_break(g, B_GLASS);
    CHECK(g->wound_count == 3 && g->wounds[0].part == BP_RARM);

    mem_free(h);
    mem_free(k);
    mem_free(g);
    tw_free(&t);
}

/* The UI batch: text metrics, wrapping, clipping to the buffer, and whole
 * HUD/panel/death screens fitting in it. */
static void test_ui(void)
{
    CHECK(ui_text_width("", 1) == 0.0f);
    CHECK(ui_text_width("abc", 1) == 17.0f); /* 3 x 6 px advance, no trailing gap */
    CHECK(ui_text_width("abc", 2) == 34.0f);
    CHECK(ui_text_width("ab\nabcd", 1) == 23.0f);
    CHECK(ui_scale_for(1280, 720) == 2 && ui_scale_for(640, 360) == 1 && ui_scale_for(320, 200) == 1);

    enum { MAXQ = 8192 };
    ui_vertex *mem = mem_alloc(sizeof(ui_vertex) * 4 * MAXQ);
    ui u;
    ui_begin(&u, mem, MAXQ, 1280, 720);
    CHECK(u.w == 640.0f && u.h == 360.0f);
    CHECK(ui_text(&u, 0, 0, 1, 0xffffffffu, "a b") == 17.0f);
    CHECK(u.quads == 2); /* spaces draw nothing */
    int lines = ui_text_wrap(&u, 0, 0, 59, 5, 1, 0xffffffffu, "one two three four five six");
    CHECK(lines == 3); /* 10 columns: "one two", "three four", "five six" */
    CHECK(ui_text_wrap(&u, 0, 0, 59, 2, 1, 0xffffffffu, "one two three four five six") == 2);
    CHECK(ui_text_wrap(&u, 0, 0, 29, 3, 1, 0xffffffffu, "unbreakableword") == 3); /* hard breaks */

    /* A full buffer drops quads instead of writing past it. */
    ui small;
    ui_begin(&small, mem, 3, 640, 360);
    ui_text(&small, 0, 0, 1, 0xffffffffu, "abcdef");
    CHECK(small.quads == 3 && small.overflow == 3);
    ui none;
    ui_begin(&none, NULL, MAXQ, 640, 360);
    ui_rect(&none, 0, 0, 10, 10, 0xffffffffu);
    CHECK(none.quads == 0);

    /* Real screens, badly hurt, fit the renderer's buffer. */
    health *h = new_body(12);
    health_env e = calm_env();
    health_cut(h, BP_LLEG, 0.8f, 1, 0.3f);
    health_cut(h, BP_LARM, 0.4f, 0, 0.3f);
    health_break_bone(h, BP_RARM, 0);
    health_break_bone(h, BP_RLEG, 1);
    live(h, &e, 30.0);
    hud_state s = {0};
    s.held = "stone";
    ui_begin(&u, mem, MAXQ, 1280, 720);
    hud_draw(&u, h, &s);
    CHECK(u.overflow == 0 && u.quads > 200);
    s.panel = 1;
    for (s.sel = 0; s.sel < BP_COUNT; s.sel++) {
        ui_begin(&u, mem, MAXQ, 1280, 720);
        hud_draw(&u, h, &s);
        CHECK(u.overflow == 0 && u.quads > 1000);
    }
    health_env drown = calm_env();
    drown.airway = AIRWAY_WATER;
    drown.submerged = 1.0;
    live(h, &drown, 600.0);
    CHECK(h->dead);
    ui_begin(&u, mem, MAXQ, 1280, 720);
    hud_draw(&u, h, &s);
    CHECK(u.overflow == 0 && u.quads > 20);
    mem_free(h);
    mem_free(mem);
}

static int test_accept(void *user, int id, int count) { return count - inv_add(user, id, count); }

/* Field by field: the struct has padding, so memcmp is not a comparison. */
static int inv_equal(const inventory *a, const inventory *b)
{
    if (a->selected != b->selected || a->cursor.id != b->cursor.id || a->cursor.count != b->cursor.count) return 0;
    for (int i = 0; i < INV_SLOTS; i++)
        if (a->slot[i].id != b->slot[i].id || a->slot[i].count != b->slot[i].count) return 0;
    return 1;
}

static void test_inventory(void)
{
    inventory inv;
    inv_init(&inv);
    /* Stacks fill up to their size, the hotbar first; the rest overflows. */
    CHECK(inv_add(&inv, B_STONE, 100) == 0);
    CHECK(inv.slot[0].id == B_STONE && inv.slot[0].count == 64 && inv.slot[1].count == 36);
    CHECK(inv_add(&inv, I_BUCKET, 3) == 0 && inv.slot[2].count == 1 && inv.slot[4].id == I_BUCKET);
    CHECK(inv_add(&inv, B_WATER, 1) == 1 && inv_add(&inv, B_BEDROCK, 2) == 2); /* not items */
    CHECK(inv_count(&inv, B_STONE) == 100);
    inventory full;
    inv_init(&full);
    CHECK(inv_add(&full, B_DIRT, 64 * INV_SLOTS + 5) == 5);
    /* Taking empties the backpack end first. */
    CHECK(inv_take(&inv, B_STONE, 40) == 40 && inv.slot[0].count == 60 && inv.slot[1].count == 0 &&
          inv.slot[1].id == 0);
    CHECK(inv_take(&inv, B_STONE, 1000) == 60 && inv_count(&inv, B_STONE) == 0);

    /* Clicks: pick up, split, place one, merge, swap, shift-move. */
    inv_init(&inv);
    inv_add(&inv, B_SAND, 10);
    inv_click(&inv, 0, 1, 0); /* right on 10: take 5 */
    CHECK(inv.cursor.count == 5 && inv.slot[0].count == 5);
    inv_click(&inv, 9, 1, 0); /* right on empty: place one */
    CHECK(inv.slot[9].id == B_SAND && inv.slot[9].count == 1 && inv.cursor.count == 4);
    inv_click(&inv, 0, 0, 0); /* left on same: merge */
    CHECK(inv.slot[0].count == 9 && inv.cursor.count == 0 && inv.cursor.id == 0);
    inv_add(&inv, B_LOG, 3); /* goes to slot 1 */
    inv_click(&inv, 1, 0, 0);
    CHECK(inv.cursor.id == B_LOG && inv.slot[1].count == 0);
    inv_click(&inv, 0, 0, 0); /* swap with the sand */
    CHECK(inv.slot[0].id == B_LOG && inv.cursor.id == B_SAND && inv.cursor.count == 9);
    item_stack left = inv_return_cursor(&inv);
    CHECK(left.count == 0 && inv_count(&inv, B_SAND) == 10);
    inv_click(&inv, 0, 0, 1); /* shift: hotbar to backpack */
    CHECK(inv.slot[0].count == 0 && inv_count(&inv, B_LOG) == 3);
    for (int i = 0; i < INV_HOTBAR; i++) CHECK(inv.slot[i].id != B_LOG);

    /* Crafting: needs the inputs, fire where it says so, and room. */
    inv_init(&inv);
    inv_add(&inv, B_LOG, 1);
    CHECK(inv_craft(&inv, 0, 0) && inv_count(&inv, B_PLANKS) == 4 && inv_count(&inv, B_LOG) == 0);
    CHECK(!inv_craft(&inv, 0, 0));
    int glass = -1;
    for (int r = 0; r < g_recipe_count; r++)
        if (g_recipes[r].out.id == B_GLASS) glass = r;
    CHECK(glass >= 0);
    inv_add(&inv, B_SAND, 2);
    CHECK(!inv_can_craft(&inv, glass, 0) && inv_can_craft(&inv, glass, 1));
    CHECK(inv_craft(&inv, glass, 1) && inv_count(&inv, B_GLASS) == 1 && inv_count(&inv, B_SAND) == 0);

    /* The kit: supplies in the backpack, an empty hand. */
    inv_starting_kit(&inv);
    CHECK(inv_count(&inv, I_BANDAGE) == 3 && inv_count(&inv, I_APPLE) == 3 && inv.slot[0].count == 0);

    /* Encoding round-trips; corrupt bytes are rejected untouched. */
    inv.selected = 4;
    inv.cursor = (item_stack){I_STICK, 7};
    uint8_t buf[INV_ENCODED_SIZE];
    CHECK(inv_encode(&inv, buf, sizeof buf) == INV_ENCODED_SIZE);
    inventory back;
    CHECK(inv_decode(&back, buf, sizeof buf) == 0 && inv_equal(&back, &inv));
    buf[8] = B_BEDROCK;
    buf[9] = 1;
    CHECK(inv_decode(&back, buf, sizeof buf) == -1);
    inv_encode(&inv, buf, sizeof buf);
    buf[8 + 2 * 12 + 1] = 200; /* 200 bandages in a stack of 16 */
    CHECK(inv_decode(&back, buf, sizeof buf) == -1);
    CHECK(inv_decode(&back, buf, sizeof buf - 1) == -1);

    /* The player file. */
    player p;
    memset(&p, 0, sizeof p);
    p.pos = dv3(-123.25, 70.5, 9999.75);
    p.yaw = 1.25f;
    p.pitch = -0.5f;
    p.flying = 1;
    uint8_t pf[PLAYER_FILE_SIZE];
    CHECK(survival_encode_player(&p, &inv, 0.75, pf, sizeof pf) == PLAYER_FILE_SIZE);
    player q;
    inventory qi;
    double day = 0.0;
    CHECK(survival_decode_player(&q, &qi, &day, pf, sizeof pf) == 0);
    CHECK(fabs(day - 0.75) < 1e-4);
    CHECK(q.pos.x == p.pos.x && q.pos.y == p.pos.y && q.pos.z == p.pos.z && q.yaw == p.yaw && q.flying == 1);
    CHECK(inv_equal(&qi, &inv));
    double nan = (double)NAN;
    memcpy(pf + 16, &nan, 8);
    CHECK(survival_decode_player(&q, &qi, NULL, pf, sizeof pf) == -1);
}

static void test_items_physics(void)
{
    test_world t;
    tw_init(&t);
    player p;
    player_spawn(&p, &t.w, 0.5, 0.5);
    t.ph.pl = &p;
    /* Dropped from 3 m, an item falls, bounces a little and comes to rest
     * on the ground. */
    CHECK(physics_drop_item(&t.ph, dv3(8.5, GROUND + 3.0, 8.5), dv3(0, 0, 0), B_STONE, 1, 0.0f));
    player_input idle = {0};
    double peak_after = 0.0;
    int landed = 0;
    for (int i = 0; i < 180; i++) {
        physics_step(&t.ph, &p, &idle);
        const item_ent *it = &t.ph.items[0];
        if (it->on_ground) landed = 1;
        else if (landed && it->pos.y > peak_after) peak_after = it->pos.y;
    }
    CHECK(t.ph.item_count == 1 && fabs(t.ph.items[0].pos.y - (GROUND + ITEM_HALF)) < 0.01);
    CHECK(peak_after > GROUND + ITEM_HALF + 0.01); /* it bounced */
    /* Two identical stacks lying together merge. */
    CHECK(physics_drop_item(&t.ph, dv3(8.7, GROUND + 0.5, 8.5), dv3(0, 0, 0), B_STONE, 2, 0.0f));
    run_steps(&t, &p, &idle, 60);
    CHECK(t.ph.item_count == 1 && t.ph.items[0].count == 3);
    /* A log floats, a stone sinks. */
    for (int x = 20; x < 23; x++)
        for (int z = 20; z < 23; z++)
            for (int y = GROUND - 3; y < GROUND; y++) world_set(&t.w, x, y, z, B_WATER, 0);
    t.ph.item_count = 0;
    physics_drop_item(&t.ph, dv3(21.5, GROUND + 0.5, 21.5), dv3(0, 0, 0), B_LOG, 1, 0.0f);
    physics_drop_item(&t.ph, dv3(21.2, GROUND + 0.5, 20.7), dv3(0, 0, 0), B_STONE, 1, 0.0f);
    run_steps(&t, &p, &idle, 400);
    CHECK(t.ph.item_count == 2);
    for (int i = 0; i < t.ph.item_count; i++) {
        const item_ent *it = &t.ph.items[i];
        if (it->id == B_LOG) CHECK(it->pos.y > GROUND - 0.5);
        else CHECK(it->pos.y < GROUND - 2.5);
    }
    /* The magnet pulls a nearby item in, and pickup hands it over. */
    t.ph.item_count = 0;
    p.pos = dv3(5.5, GROUND, 5.5);
    physics_drop_item(&t.ph, dv3(6.7, GROUND + 0.2, 5.5), dv3(0, 0, 0), I_APPLE, 2, 0.1f);
    t.ph.magnet = 1;
    inventory inv;
    inv_init(&inv);
    int got = 0;
    for (int i = 0; i < 120 && !got; i++) {
        physics_step(&t.ph, &p, &idle);
        got = physics_pickup(&t.ph, test_accept, &inv);
    }
    CHECK(got == 1 && inv_count(&inv, I_APPLE) == 2 && t.ph.item_count == 0);
    /* A full inventory leaves the item on the ground. */
    inventory full;
    inv_init(&full);
    inv_add(&full, B_DIRT, 64 * INV_SLOTS);
    physics_drop_item(&t.ph, dv3(5.5, GROUND + 0.9, 5.5), dv3(0, 0, 0), I_APPLE, 1, 0.0f);
    run_steps(&t, &p, &idle, 10);
    CHECK(physics_pickup(&t.ph, test_accept, &full) == 0 && t.ph.item_count == 1);
    tw_free(&t);
}

static void test_interact(void)
{
    test_world t;
    tw_init(&t);
    fx_state *fx = mem_alloc(sizeof *fx);
    fx_init(fx, 5);
    player p;
    player_spawn(&p, &t.w, 0.5, 0.5);
    p.pitch = -1.2f;
    t.ph.pl = &p;
    inventory inv;
    inv_init(&inv);
    interact s;
    interact_init(&s, 3);
    dvec3 eye = dv3(p.pos.x, p.pos.y + PLAYER_EYE, p.pos.z);
    vec3 dir = look_dir(p.yaw, p.pitch);
    /* Holding the button on grass (0.9 s) breaks it after about that long,
     * and drops dirt. */
    interact_input hold = {.attack = 1, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    float held = 0.0f;
    int broke = 0;
    for (int i = 0; i < 200 && !broke; i++) {
        ray_hit hit = physics_raycast(&t.w, eye, dir, 5.0);
        interact_out o = interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, dir, hit, &hold);
        held += hold.dt;
        broke = o.broke;
        if (o.broke) CHECK(o.broken_id == B_GRASS);
    }
    CHECK(broke && held > 0.85f && held < 0.95f);
    CHECK(t.ph.item_count == 1 && t.ph.items[0].id == B_DIRT && fx->count >= 12);
    /* Letting go resets the progress. */
    ray_hit hit = physics_raycast(&t.w, eye, dir, 5.0);
    for (int i = 0; i < 20; i++) interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, dir, hit, &hold);
    CHECK(s.progress > 0.0f);
    interact_input none = {.can_act = 1, .dt = 1.0f / 60.0f};
    interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, dir, hit, &none);
    CHECK(s.progress == 0.0f);
    /* Placing uses up the held block; with nothing to place nothing happens. */
    inv_add(&inv, B_PLANKS, 2);
    interact_input use = {.use_click = 1, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    hit = physics_raycast(&t.w, eye, dir, 5.0);
    interact_out o = interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, dir, hit, &use);
    CHECK(o.actions == 1 && inv_count(&inv, B_PLANKS) == 1 &&
          world_get(&t.w, hit.before.x, hit.before.y, hit.before.z) == B_PLANKS);
    /* Buckets carry a whole cell of water: scooping and pouring conserve it. */
    for (int x = 6; x <= 8; x++)
        for (int z = -4; z <= -2; z++) world_set(&t.w, x, GROUND - 1, z, B_WATER, 0);
    physics_fluid_tick(&t.ph);
    inv_init(&inv);
    inv_add(&inv, I_BUCKET, 1);
    p.pos = dv3(7.5, GROUND, 0.5);
    eye = dv3(p.pos.x, p.pos.y + PLAYER_EYE, p.pos.z);
    p.pitch = -0.55f;
    dir = look_dir(p.yaw, p.pitch);
    hit = physics_raycast(&t.w, eye, dir, 5.0);
    interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, dir, hit, &use);
    CHECK(inv_held(&inv)->id == I_WATER_BUCKET);
    int water = 0;
    for (int x = 6; x <= 8; x++)
        for (int z = -4; z <= -2; z++) water += world_water_level(&t.w, x, GROUND - 1, z);
    CHECK(water == 8 * 8); /* exactly one cell's worth left the pool */
    p.pitch = -1.0f; /* the dry grass in front of the pool */
    dir = look_dir(p.yaw, p.pitch);
    hit = physics_raycast(&t.w, eye, dir, 5.0);
    interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, dir, hit, &use); /* pour it back out */
    CHECK(inv_held(&inv)->id == I_BUCKET && hit.hit &&
          world_get(&t.w, hit.before.x, hit.before.y, hit.before.z) == B_WATER);
    inv_set_held(&inv, I_WATER_BUCKET, 1);
    /* Dropping throws the held stack forward as an item. */
    int before = t.ph.item_count;
    interact_input drop = {.drop = 2, .can_act = 1, .dt = 1.0f / 60.0f};
    interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, dir, hit, &drop);
    CHECK(t.ph.item_count == before + 1 && inv_held(&inv)->count == 0);
    /* The entity list: items and particles, opaque first. */
    entity_instance *ents = mem_alloc(sizeof(entity_instance) * 2048);
    int no = 0, nt = 0;
    fx_crack cr = {1, 0, GROUND - 1, 0, 0.6f};
    fx_build_entities(fx, &t.ph, &cr, eye, 0.5, 1.0f, ents, 2048, &no, &nt);
    CHECK(no > 0 && nt >= 2); /* the bucket card and the crack overlay */
    CHECK(ents[no + nt - 1].tex == (uint32_t)(T_CRACK0 + 2) * 0x010101u);
    fx_step(fx, &t.w, 5.0); /* everything has expired */
    CHECK(fx->count == 0);
    mem_free(ents);
    mem_free(fx);
    tw_free(&t);
}

static void test_settings(void)
{
    settings s, t;
    settings_default(&s);
    CHECK(s.fov == 70 && s.sensitivity == 100 && s.vsync == 1 && s.gui_scale == 0);
    char buf[1024];
    s.fov = 95;
    s.sprint_toggle = 1;
    s.gui_scale = 3;
    int n = settings_format(&s, buf, (int)sizeof buf);
    CHECK(n > 0 && n < (int)sizeof buf);
    settings_default(&t);
    CHECK(settings_parse(&t, buf) == 10);
    CHECK(memcmp(&s, &t, sizeof s) == 0);
    /* Out of range, junk and unknown keys are ignored; the rest applies. */
    settings_default(&t);
    CHECK(settings_parse(&t, "fov 500\nfov 12x\nbogus 3\n# fov 80\nsensitivity 250\r\nvsync -1\n fov 60\n") == 1);
    CHECK(t.fov == 70 && t.sensitivity == 250 && t.vsync == 1);
    /* A tiny buffer truncates instead of overflowing. */
    char tiny[8];
    CHECK(settings_format(&s, tiny, (int)sizeof tiny) == 7 && tiny[7] == '\0');
}

/* Drives a menu with scripted input: n frames of dt, optional click at (x, y). */
static menu_action menu_run(menu *m, ui *u, ui_vertex *mem, settings *s, const menu_input *in0, int frames)
{
    menu_action last = MENU_NONE;
    for (int i = 0; i < frames; i++) {
        menu_input in = *in0;
        if (i) in.click = in.up = in.down = in.left = in.right = in.enter = in.back = 0;
        ui_begin(u, mem, 8192, 1280, 720);
        menu_action a = menu_frame(m, u, &in, s);
        CHECK(u->overflow == 0);
        if (a != MENU_NONE) last = a;
    }
    return last;
}

static void test_menu(void)
{
    ui_vertex *mem = mem_alloc(sizeof(ui_vertex) * 4 * 8192);
    ui u;
    settings s;
    settings_default(&s);
    menu m;
    menu_init(&m);
    menu_input idle = {.mx = -1, .my = -1, .dt = 1.0f / 60.0f};

    /* Title: the first button is Play and has the focus; Enter plays. */
    CHECK(menu_run(&m, &u, mem, &s, &idle, 30) == MENU_NONE);
    menu_input enter = idle;
    enter.enter = 1;
    CHECK(menu_run(&m, &u, mem, &s, &enter, 1) == MENU_PLAY);
    /* Down, Enter: Settings. Esc goes back to the title. */
    menu_input down = idle;
    down.down = 1;
    menu_run(&m, &u, mem, &s, &down, 1);
    CHECK(menu_run(&m, &u, mem, &s, &enter, 1) == MENU_NONE && m.screen == SCREEN_SETTINGS);
    menu_run(&m, &u, mem, &s, &idle, 30);
    /* The first row is the FOV slider: Right raises it and reports a change. */
    menu_input right = idle;
    right.right = 1;
    CHECK(menu_run(&m, &u, mem, &s, &right, 1) == MENU_SETTINGS && s.fov == 71);
    /* Pressing a slider row at its left edge drags it to the minimum; scan
     * down the panel for the FOV row. */
    int found = 0;
    for (int y = 40; y < 200 && !found; y += 2) {
        settings before = s;
        menu_input c = idle;
        c.mx = 180;
        c.my = (float)y;
        c.click = c.mouse_down = 1;
        ui_begin(&u, mem, 8192, 1280, 720);
        menu_frame(&m, &u, &c, &s);
        if (s.fov == SETTINGS_FOV_MIN) found = 1;
        else s = before;
        c.click = c.mouse_down = 0;
        ui_begin(&u, mem, 8192, 1280, 720);
        menu_frame(&m, &u, &c, &s); /* release */
    }
    CHECK(found);
    menu_input back = idle;
    back.back = 1;
    menu_run(&m, &u, mem, &s, &back, 1);
    CHECK(m.screen == SCREEN_TITLE);

    /* Pause: Esc resumes; the controls screen returns to pause. */
    menu_open(&m, SCREEN_PAUSE);
    CHECK(menu_run(&m, &u, mem, &s, &back, 1) == MENU_RESUME);
    menu_open(&m, SCREEN_CONTROLS);
    menu_run(&m, &u, mem, &s, &idle, 20);
    menu_run(&m, &u, mem, &s, &back, 1);
    CHECK(m.screen == SCREEN_PAUSE);
    /* Clicking the last pause button quits. */
    menu_run(&m, &u, mem, &s, &idle, 30);
    menu_action got = MENU_NONE;
    for (int y = 150; y < 300 && got == MENU_NONE; y += 4) {
        menu_input c = idle;
        c.mx = 320;
        c.my = (float)y;
        c.click = 1;
        got = menu_run(&m, &u, mem, &s, &c, 1);
        if (got != MENU_NONE && got != MENU_QUIT) {
            menu_open(&m, SCREEN_PAUSE);
            menu_run(&m, &u, mem, &s, &idle, 30);
            got = MENU_NONE;
        }
    }
    CHECK(got == MENU_QUIT);

    /* The F3 overlay fits the buffer. */
    debug_frames f;
    memset(&f, 0, sizeof f);
    for (int i = 0; i < 500; i++) debug_frames_push(&f, 10.0f + (float)(i % 7));
    CHECK(f.fps > 70.0f && f.fps < 100.0f);
    debug_info d;
    memset(&d, 0, sizeof d);
    d.frames = &f;
    d.has_target = 1;
    d.target_id = B_WATER;
    d.target_level = 5;
    ui_begin(&u, mem, 8192, 1280, 720);
    debug_draw(&u, &d);
    CHECK(u.overflow == 0 && u.quads > 300);
    CHECK(!strcmp(debug_facing(0.0f), "north") && !strcmp(debug_facing((float)MC_PI * 0.5f), "east") &&
          !strcmp(debug_facing(-(float)MC_PI * 0.75f), "south-west"));
    mem_free(mem);
}

int main(void)
{
    mem_init();
    mesher_init();
    test_gpupool();
    test_save_roundtrip();
    test_save_files();
    test_mesher();
    test_world_basics();
    test_player_physics();
    test_friction();
    test_sprint_sneak();
    test_camera();
    test_structure();
    test_buoyancy();
    test_settle();
    test_fluid();
    test_raycast();
    test_health_rest();
    test_health_exercise();
    test_health_bleeding();
    test_health_drowning();
    test_health_fractures();
    test_health_infection();
    test_health_needs();
    test_survival();
    test_ui();
    test_settings();
    test_menu();
    test_inventory();
    test_items_physics();
    test_interact();
    test_thermo_all();
    test_health2_all();
    test_bodies_all();
    test_visual_all();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
