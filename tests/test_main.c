/* Unit tests for the engine core (no GPU, no window). */
#include "gpupool.h"
#include "jobs.h"
#include "mem.h"
#include "mesher.h"
#include "physics.h"
#include "save.h"
#include "world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failed, g_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
    } while (0)

#define GROUND 13 /* first air block in the flat test world */

static void flat_gen(uint32_t seed, column *c)
{
    (void)seed;
    memset(c->blocks, B_AIR, COL_VOL);
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++)
            for (int y = 0; y < GROUND; y++)
                c->blocks[col_index(x, y, z)] = y == 0 ? B_BEDROCK : (y < 10 ? B_STONE : (y < 12 ? B_DIRT : B_GRASS));
}

typedef struct {
    jobs *js;
    world w;
    physics ph;
} test_world;

static void tw_init(test_world *t)
{
    t->js = jobs_create(0);
    world_init(&t->w, 1, 3, t->js, NULL);
    t->w.generator = flat_gen;
    world_load_blocking(&t->w, 0.5, 0.5, 3);
    physics_init(&t->ph, &t->w);
    t->w.edit_user = &t->ph;
    t->w.on_block_changed = physics_on_block_changed;
}

static void tw_free(test_world *t)
{
    physics_destroy(&t->ph);
    world_destroy(&t->w);
    jobs_destroy(t->js);
}

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
    flat_gen(0, c);
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
    fclose(f);
    return n == strlen(text) && memcmp(buf, text, n) == 0;
}

/* A shared world folder must not be able to redirect saves elsewhere. */
static void test_save_files(void)
{
    char dir[] = "/tmp/mc_test_XXXXXX";
    if (!mkdtemp(dir)) {
        CHECK(!"mkdtemp");
        return;
    }
    char victim[128], p[160];
    snprintf(victim, sizeof victim, "%s/victim.txt", dir);
    FILE *f = fopen(victim, "wb");
    fputs("precious", f);
    fclose(f);

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
    flat_gen(0, c);
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
    column *c = world_column(&t.w, 0, 0);
    CHECK(c && c->modified && c->unsaved && c->dirty[0]);
    /* Edits on a column border dirty the neighbour's section too. */
    column *n = world_column(&t.w, -1, 0);
    n->dirty[0] = 0;
    world_set(&t.w, 0, 13, 5, B_BRICK, 0);
    CHECK(n->dirty[0]);
    tw_free(&t);
}

static void run_steps(test_world *t, player *p, const player_input *in, int n)
{
    for (int i = 0; i < n; i++) physics_step(&t->ph, p, in);
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
    test_structure();
    test_buoyancy();
    test_settle();
    test_fluid();
    test_raycast();
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
