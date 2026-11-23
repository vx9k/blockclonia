/* The world's edge at +-WORLD_LIMIT, 2e9 blocks out: the terrain is as
 * fine near the edge as at the origin, the last columns load, mesh, save and stream
 * like any other, bodies stop against the edge instead of snagging on it
 * or passing it, and the decoders take positions up to the edge and
 * refuse anything beyond. A host steps players beside the edge and puts
 * those joining there inside it. */
#include "fx.h"
#include "mem.h"
#include "mp.h"
#include "netcrypto.h"
#include "noise.h"
#include "save.h"
#include "save_db.h"
#include "survival.h"
#include "test_util.h"
#include "thermo.h"
#include "wire.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EDGE WORLD_LIMIT
#define EDGE_COLS (WORLD_LIMIT / CHUNK_W) /* columns from the origin to the edge */

/* The noise takes its coordinates in double: by the edge, where floats
 * are 128 blocks apart, one block is still a small step of its own. */
static void test_exact_noise(void)
{
    float step = 0.0f, step3 = 0.0f;
    int stuck = 0;
    for (int i = 0; i < 2000; i++) {
        int x = i < 1000 ? EDGE - 1001 + i : -EDGE + i - 1000, z = -EDGE + 7;
        float d = fabsf(noise2d(7, (x + 1) / 90.0, z / 90.0) - noise2d(7, x / 90.0, z / 90.0));
        float d3 = fabsf(noise3d(7, (x + 1) / 44.0, 0.3, z / 44.0) - noise3d(7, x / 44.0, 0.3, z / 44.0));
        step = d > step ? d : step;
        step3 = d3 > step3 ? d3 : step3;
        stuck += d < 1e-6f;
    }
    CHECK(step < 0.05f && step3 < 0.1f && stuck < 10);
}

typedef struct {
    int max_step, flat, lo, hi;
} relief;

/* Surface heights of the 128 x 128 blocks from (x0, z0): the largest step
 * between neighbours, how many steps are flat, the lowest and highest. */
static relief relief_at(int x0, int z0)
{
    int *h = mem_alloc(sizeof(int) * 129 * 129);
    for (int z = 0; z < 129; z++)
        for (int x = 0; x < 129; x++) h[z * 129 + x] = worldgen_height(1337, x0 + x, z0 + z);
    relief r = {0, 0, INT_MAX, INT_MIN};
    for (int z = 0; z < 128; z++)
        for (int x = 0; x < 128; x++) {
            int at = h[z * 129 + x], a = abs(h[z * 129 + x + 1] - at), b = abs(h[(z + 1) * 129 + x] - at);
            r.max_step = a > r.max_step ? a : r.max_step;
            r.max_step = b > r.max_step ? b : r.max_step;
            r.flat += (a == 0) + (b == 0);
            r.lo = at < r.lo ? at : r.lo;
            r.hi = at > r.hi ? at : r.hi;
        }
    mem_free(h);
    return r;
}

/* A generated column: bedrock at the bottom, valid blocks, and its top
 * where the surface height says (give or take trees, water and caves). */
static int column_sound(int cx, int cz)
{
    column *c = column_alloc(cx, cz);
    worldgen_column(1337, c);
    int ok = 1, near = 0;
    for (int i = 0; i < COL_VOL; i++) ok &= c->blocks[i] < B_COUNT;
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) {
            ok &= c->blocks[col_index(x, 0, z)] == B_BEDROCK;
            int top = WORLD_H - 1;
            while (top > 0 && c->blocks[col_index(x, top, z)] == B_AIR) top--;
            int h = worldgen_height(1337, cx * CHUNK_W + x, cz * CHUNK_W + z);
            near += top >= h - 1 && top <= (h > SEA_LEVEL ? h : SEA_LEVEL) + 12;
        }
    column_free(c);
    return ok && near >= 200;
}

/* The share of the ground under the surface that caves hollow out, over
 * the 4 x 4 columns from (cx0, cz0). */
static double cave_share(int cx0, int cz0)
{
    long air = 0, all = 0;
    for (int cz = cz0; cz < cz0 + 4; cz++)
        for (int cx = cx0; cx < cx0 + 4; cx++) {
            column *c = column_alloc(cx, cz);
            worldgen_column(1337, c);
            for (int z = 0; z < CHUNK_W; z++)
                for (int x = 0; x < CHUNK_W; x++) {
                    int h = worldgen_height(1337, cx * CHUNK_W + x, cz * CHUNK_W + z);
                    for (int y = 1; y < h - 1; y++) air += c->blocks[col_index(x, y, z)] == B_AIR;
                    all += h > 2 ? h - 2 : 0;
                }
            column_free(c);
        }
    return all ? (double)air / (double)all : 0.0;
}

/* Terrain by the edge is like terrain at the origin. Sampled at float
 * coordinates it would stand in 128-block terraces, flat at 99 % of
 * neighbours; here the steps are the same size as at the origin and as
 * few of them flat. */
static void test_edge_terrain(void)
{
    static const int AT[][2] = {
        {-64, -64},
        {EDGE - 129, EDGE - 129},
        {-EDGE + 1000, -EDGE},
        {EDGE - 2129, -EDGE + 5000},
        {-EDGE + 79777, EDGE - 129},
    };
    for (size_t i = 0; i < sizeof AT / sizeof AT[0]; i++) {
        relief r = relief_at(AT[i][0], AT[i][1]);
        CHECK(r.max_step <= 12);
        CHECK(r.flat < 2 * 128 * 128 * 97 / 100);
        CHECK(r.hi - r.lo >= 3 && r.lo >= 4 && r.hi <= WORLD_H - 12);
    }
    CHECK(column_sound(0, 0));
    CHECK(column_sound(EDGE_COLS - 1, EDGE_COLS - 1));
    CHECK(column_sound(-EDGE_COLS, -EDGE_COLS));
    CHECK(column_sound(EDGE_COLS - 1, -EDGE_COLS));

    /* Caves are where two 3D noise fields are both near zero. Sampled at
     * float coordinates by the edge the fields would stand still across
     * 128 blocks and hollow out up to a fifth of the rock; here 3 to 8 %
     * of it is cave, as at the origin. */
    double share = cave_share(EDGE_COLS - 4, EDGE_COLS - 4) + cave_share(-EDGE_COLS, -EDGE_COLS) +
                   cave_share(-EDGE_COLS + 100, EDGE_COLS - 4) + cave_share(EDGE_COLS - 4, -EDGE_COLS + 3000);
    CHECK(share / 4 > 0.02 && share / 4 < 0.1);
    share = cave_share(-1, -1);
    CHECK(share > 0.02 && share < 0.1);
}

static void stream(world *w, double x, double z)
{
    for (int i = 0; i < 400; i++) {
        world_update(w, x, GROUND + 2.0, z);
        jobs_poll(w->jobs, -1);
        world_schedule(w);
    }
}

/* Every column of the ring around (x, z) inside the world is loaded. */
static int ring_loaded(const world *w, double x, double z)
{
    int cx = chunk_of((int)floor(x)), cz = chunk_of((int)floor(z)), all = 1;
    for (int dz = -w->radius; dz <= w->radius; dz++)
        for (int dx = -w->radius; dx <= w->radius; dx++)
            if (dx * dx + dz * dz <= w->radius * w->radius &&
                world_xz_inside((long long)(cx + dx) * CHUNK_W, (long long)(cz + dz) * CHUNK_W))
                all &= world_column(w, cx + dx, cz + dz) != NULL;
    return all;
}

/* The last column and the one beyond: loaded and editable, and never. */
static void test_edge_world(void)
{
    jobs *js = jobs_create(0);
    world w = {0};
    world_init(&w, 1, 2, js, NULL);
    w.generator = test_flat_gen;
    w.max_gen_in_flight = 64; /* no workers: one load per update otherwise */
    world_load_blocking(&w, EDGE - 0.5, -EDGE + 0.5, 2);
    CHECK(world_column(&w, EDGE_COLS - 1, -EDGE_COLS) != NULL);
    CHECK(world_column(&w, EDGE_COLS, -EDGE_COLS) == NULL && world_column(&w, EDGE_COLS - 1, -EDGE_COLS - 1) == NULL);
    CHECK(world_get(&w, EDGE - 1, GROUND - 1, -EDGE) == B_GRASS);
    CHECK(world_get(&w, EDGE, GROUND - 1, -EDGE) == B_UNLOADED);
    CHECK(world_get(&w, EDGE - 1, GROUND - 1, -EDGE - 1) == B_UNLOADED);
    CHECK(world_get(&w, INT_MAX, GROUND, INT_MIN) == B_UNLOADED);
    CHECK(world_set(&w, EDGE - 1, GROUND, -EDGE, B_BRICK, 0) && world_surface_y(&w, EDGE - 1, -EDGE) == GROUND + 1);
    CHECK(!world_set(&w, EDGE, GROUND, -EDGE, B_BRICK, 0) && !world_set(&w, INT_MAX, GROUND, INT_MIN, B_BRICK, 0));

    /* What a host sends a client: the edge column, nothing past it. */
    uint8_t *buf = mem_alloc(SAVE_MAX_FILE);
    column *c = column_alloc(EDGE_COLS - 1, -EDGE_COLS);
    size_t n = world_column_encode(&w, EDGE_COLS - 1, -EDGE_COLS, buf, SAVE_MAX_FILE);
    CHECK(n > 0 && save_decode_column(c, buf, n) == 0 && c->blocks[col_index(15, GROUND, 0)] == B_BRICK);
    CHECK(world_column_encode(&w, -EDGE_COLS, EDGE_COLS - 1, buf, SAVE_MAX_FILE) > 0); /* generated */
    CHECK(world_column_encode(&w, EDGE_COLS, 0, buf, SAVE_MAX_FILE) == 0);
    CHECK(world_column_encode(&w, 0, -EDGE_COLS - 1, buf, SAVE_MAX_FILE) == 0);
    CHECK(world_column_encode(&w, INT_MAX, INT_MIN, buf, SAVE_MAX_FILE) == 0);
    column_free(c);
    mem_free(buf);

    /* Teleports: the columns left behind are hundreds of millions of
     * columns away (their squared distance is past an int), and give
     * their places in the ring up to the new ones. */
    stream(&w, 0.5, 0.5);
    CHECK(ring_loaded(&w, 0.5, 0.5));
    stream(&w, 800000.5, 0.5);
    CHECK(ring_loaded(&w, 800000.5, 0.5) && world_column(&w, 0, 0) == NULL);
    stream(&w, -EDGE + 0.5, -EDGE + 0.5);
    CHECK(ring_loaded(&w, -EDGE + 0.5, -EDGE + 0.5) && world_column(&w, -EDGE_COLS, -EDGE_COLS) != NULL);
    stream(&w, EDGE - 0.5, EDGE - 0.5);
    CHECK(ring_loaded(&w, EDGE - 0.5, EDGE - 0.5) && world_column(&w, -EDGE_COLS, -EDGE_COLS) == NULL);
    world_destroy(&w);
    jobs_destroy(js);
}

typedef struct {
    int cx[2], cz, got[2];
    mesh_counts mc[2];
    uint32_t verts[2][64];
} mesh_watch;

/* cppcheck-suppress constParameterCallback */
static int watch_mesh(void *user, column *c, int sy, const uint32_t *verts, const mesh_counts *mc, const mesh_extra *x)
{
    (void)x;
    mesh_watch *m = user;
    uint32_t n = (mc->opaque + mc->trans) * 4;
    for (int i = 0; i < 2; i++)
        if (sy == 0 && c->cx == m->cx[i] && c->cz == m->cz) {
            m->got[i]++;
            m->mc[i] = *mc;
            memcpy(m->verts[i], verts, sizeof(uint32_t) * (n < 64 ? n : 64));
        }
    return 1;
}

/* The last column meshes just like the one beside it on flat ground:
 * nothing beyond it will ever load to complete its neighbours, it draws no
 * wall facing out, and no shadow of one along its outer row. */
static void test_edge_mesh(void)
{
    for (int side = 0; side < 2; side++) {
        jobs *js = jobs_create(0);
        world w = {0};
        world_init(&w, 1, 3, js, NULL);
        w.generator = test_flat_gen;
        w.max_gen_in_flight = 64;
        mesh_watch m = {{0}, 0, {0}, {{0}}, {{0}}};
        m.cx[0] = side ? -EDGE_COLS : EDGE_COLS - 1; /* the last column */
        m.cx[1] = side ? -EDGE_COLS + 1 : EDGE_COLS - 2;
        m.cz = 5;
        w.render_user = &m;
        w.on_mesh_ready = watch_mesh;
        stream(&w, side ? -EDGE + 8.5 : EDGE - 8.5, 5 * CHUNK_W + 8.5);
        CHECK(m.got[0] >= 1 && m.got[1] >= 1);
        CHECK(m.mc[0].opaque == m.mc[1].opaque && m.mc[0].opaque > 0 && m.mc[0].trans == 0);
        CHECK(m.mc[0].face_end[1] == 0); /* only the grass tops: no +X or -X faces */
        CHECK(memcmp(m.verts[0], m.verts[1], sizeof m.verts[0]) == 0);
        world_destroy(&w);
        jobs_destroy(js);
    }
}

/* A stone wall along z at x = wx (blocks wx .. wx + 1 high from GROUND). */
static void build_wall(world *w, int wx, int z0, int z1)
{
    for (int z = z0; z <= z1; z++)
        for (int y = GROUND; y < GROUND + 2; y++) world_set(w, wx, y, z, B_STONE, 0);
}

/* Walks diagonally into a wall facing dir (+1: the wall's west face at
 * x = face, -1: its east face) and along it. 1 if the player stopped at
 * the face without entering it and kept sliding. */
static int slides_along(test_world *t, double face, int dir, double start_x, double start_z)
{
    player p = {0};
    player_spawn(&p, &t->w, start_x, start_z);
    p.pos.x = start_x;
    p.prev_pos = p.pos;
    double z0 = p.pos.z;
    const player_input in = {.forward = 1.0f, .right = (float)dir}; /* -z, and +x or -x */
    int entered = 0;
    for (int i = 0; i < 90; i++) {
        physics_step(&t->ph, &p, &in);
        entered |= dir > 0 ? p.pos.x + PLAYER_HALF_W > face + 1e-6 : p.pos.x - PLAYER_HALF_W < face - 1e-6;
    }
    double gap = dir > 0 ? face - (p.pos.x + PLAYER_HALF_W) : (p.pos.x - PLAYER_HALF_W) - face;
    return !entered && gap < 1e-5 && z0 - p.pos.z > 3.0 && p.on_ground;
}

/* Bodies at the edge, where doubles are 2^-22 m apart: the player walks
 * into the edge and stops, slides along walls by it, falls and lands;
 * dropped items and falling blocks come to rest against the edge. */
static void test_edge_physics(void)
{
    test_world t = {0};
    tw_init(&t);
    for (int side = 0; side < 2; side++) {
        int dir = side ? -1 : 1;
        double edge = dir * (double)EDGE;
        int last = side ? -EDGE : EDGE - 1; /* the last block column */
        world_load_blocking(&t.w, edge - dir * 8.5, 0.5, 3);

        player p = {0};
        player_spawn(&p, &t.w, edge - dir * 3.5, 0.5);
        const player_input push = {.right = (float)dir};
        run_steps(&t, &p, &push, 120);
        CHECK(fabs(p.pos.x - (edge - dir * PLAYER_HALF_W)) < 1e-5 && p.hit_wall && p.on_ground);
        CHECK(dir * p.pos.x <= EDGE - PLAYER_HALF_W + 1e-6);

        /* Falling 6 m against the edge: lands on the ground, still at it. */
        double x = p.pos.x;
        p.pos.y = GROUND + 6.0;
        p.vel = dv3(0, 0, 0);
        const player_input idle = {0};
        run_steps(&t, &p, &idle, 120);
        CHECK(p.on_ground && p.pos.y == GROUND && fabs(p.pos.x - x) < 1e-6);

        /* Along the edge and along walls just inside it, from starts
         * spread over a block so the faces meet every rounding. */
        int ok = 0, tries = 0;
        for (int k = 0; k < 6; k++) {
            int wx = side ? -EDGE + 2 + 3 * k : EDGE - 3 - 3 * k;
            build_wall(&t.w, wx, -40, 40);
            double face = side ? wx + 1.0 : (double)wx;
            ok += slides_along(&t, face, dir, face - dir * (1.13 + 0.137 * k), 20.5);
            tries++;
        }
        ok += slides_along(&t, edge, dir, edge - dir * 1.4, 20.5);
        tries++;
        CHECK(ok == tries);

        /* An item thrown at the edge and a falling block pushed into it. */
        t.ph.pls[0] = &p;
        t.ph.pl_count = 1;
        t.ph.item_count = 0;
        CHECK(physics_drop_item(&t.ph, dv3(edge - dir * 0.5, GROUND + 2.0, -30.5), dv3(dir * 6.0, 0, 0), B_STONE, 1,
                                10.0f));
        t.ph.bodies[0] = (body){.pos = dv3(last, GROUND + 4, -20),
                                .prev_pos = dv3(last, GROUND + 4, -20),
                                .vel = dv3(dir * 3.0, 0, 0),
                                .block = B_SAND};
        t.ph.body_count = 1;
        run_steps(&t, &p, &idle, 300);
        const item_ent *it = &t.ph.items[0];
        CHECK(t.ph.item_count == 1 && it->on_ground && fabs(it->pos.y - (GROUND + ITEM_HALF)) < 0.01);
        CHECK(dir * it->pos.x <= EDGE - ITEM_HALF + 1e-6 && dir * it->pos.x > EDGE - 1.0);
        CHECK(t.ph.body_count == 0 && world_get(&t.w, last, GROUND, -20) == B_SAND);
    }
    tw_free(&t);
}

/* A player saved, or sent, at the edge comes back; one beyond it, or not
 * a number, is refused. */
static void test_edge_records(void)
{
    static const double OK[] = {EDGE, EDGE - PLAYER_HALF_W, -EDGE, -EDGE + 0.5, 0.0};
    static const double BAD[] = {EDGE + 1.0, -EDGE - 1.0, EDGE + 1e-3, 1e300, (double)NAN, (double)INFINITY};
    inventory inv = {0};
    inv_init(&inv);
    uint8_t rec[PLAYER_FILE_SIZE] = {0}, sent[PLAYER_NET_BYTES] = {0};
    for (int bad = 0; bad < 2; bad++) {
        const double *xs = bad ? BAD : OK;
        size_t count = bad ? sizeof BAD / sizeof BAD[0] : sizeof OK / sizeof OK[0];
        for (size_t i = 0; i < count; i++)
            for (int axis = 0; axis < 2; axis++) {
                player p = {0}, back = {0};
                p.pos = dv3(axis ? 3.5 : xs[i], GROUND, axis ? xs[i] : -3.5);
                p.prev_pos = p.pos;
                inventory got = {0};
                size_t n = survival_encode_player(&p, &inv, 0.25, rec, sizeof rec);
                int read = n > 0 && survival_decode_player(&back, &got, NULL, rec, n) == 0;
                CHECK(read == !bad);
                if (read) CHECK(back.pos.x == p.pos.x && back.pos.z == p.pos.z);
                /* The multiplayer snapshot's copy of the player. */
                memset(&back, 0, sizeof back);
                n = player_encode(&p, sent, sizeof sent);
                read = n > 0 && player_decode(&back, sent, n) == 0;
                CHECK(read == !bad);
            }
    }
}

/* Columns edited at the edge are saved and read back. */
static void test_edge_save(void)
{
    char dir[400] = {0}, file[512] = {0};
    CHECK(tmp_dir_make(dir, sizeof dir, "mc_edge_"));
    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (!db) return;
    jobs *js = jobs_create(0);
    for (int side = 0; side < 2; side++) {
        int x = side ? -EDGE : EDGE - 1, z = side ? EDGE - 1 : -EDGE;
        uint8_t id = side ? B_BRICK : B_GLASS;
        world w = {0};
        world_init(&w, 1, 2, js, db);
        w.generator = test_flat_gen;
        world_load_blocking(&w, x + 0.5, z + 0.5, 1);
        CHECK(world_set(&w, x, GROUND, z, id, 0));
        world_save_all(&w);
        world_destroy(&w);
        uint8_t *row = NULL;
        CHECK(save_db_get_column(db, chunk_of(x), chunk_of(z), &row) > 0);
        mem_free(row);
        world_init(&w, 1, 2, js, db);
        w.generator = test_flat_gen;
        world_load_blocking(&w, x + 0.5, z + 0.5, 1);
        CHECK(world_get(&w, x, GROUND, z) == id);
        world_destroy(&w);
    }
    jobs_destroy(js);
    save_db_close(db);
    snprintf(file, sizeof file, "%s/world.db", dir);
    tmp_remove(file);
    tmp_remove(dir);
}

/* Edits a host sends: the first and last block columns are taken, any
 * past them refused along with the rest of the message. */
static void test_edge_edits(void)
{
    test_world t = {0};
    tw_init(&t);
    world_load_blocking(&t.w, EDGE - 8.5, EDGE - 8.5, 2);
    player pl = {0};
    inventory inv = {0};
    health *hl = new_body(1);
    mp_world ctx = {.w = &t.w, .ph = &t.ph, .pl = &pl, .hl = hl, .inv = &inv};
    mp *m = mp_create(NULL, &ctx);
    mp_attach(m, &ctx);
    static const int FIRST[][2] = {
        {-EDGE, -EDGE}, {EDGE - 1, EDGE - 1}, {EDGE, 0}, {0, EDGE}, {-EDGE - 1, 0}, {0, -EDGE - 1}, {INT_MAX, INT_MIN},
    };
    for (int i = 0; i < 7; i++) {
        /* Two edits: the one under test, then glass in a loaded column,
         * which is set only if the first passed. */
        uint8_t msg[32] = {0};
        wire_w wr = wire_writer(msg, sizeof msg);
        wire_u8(&wr, 18); /* H_EDITS in mp.c */
        wire_u16(&wr, 2);
        wire_i32(&wr, FIRST[i][0]);
        wire_u16(&wr, GROUND);
        wire_i32(&wr, FIRST[i][1]);
        wire_u8(&wr, B_BRICK);
        wire_u8(&wr, 0);
        wire_i32(&wr, EDGE - 2 - i);
        wire_u16(&wr, GROUND);
        wire_i32(&wr, EDGE - 9);
        wire_u8(&wr, B_GLASS);
        wire_u8(&wr, 0);
        CHECK(!wr.bad);
        mp_receive(m, 0, NET_CH_GAME, msg, wr.len);
        CHECK((world_get(&t.w, EDGE - 2 - i, GROUND, EDGE - 9) == B_GLASS) == (i < 2));
    }
    CHECK(world_get(&t.w, EDGE - 1, GROUND, EDGE - 1) == B_BRICK);
    mp_destroy(m);
    mem_free(hl);
    tw_free(&t);
}

/* Players joining a host who stands against the edge are put inside the
 * world beside them, and are stepped though the columns past the edge
 * will never load. */
static void test_edge_host(void)
{
    test_world t = {0};
    tw_init(&t);
    world_load_blocking(&t.w, EDGE - 8.5, 0.5, 2);
    thermo *th = thermo_create(1);
    fx_state *fx = mem_calloc(1, sizeof *fx);
    fx_init(fx, 1);
    health *hl = new_body(1);
    inventory inv = {0};
    inv_init(&inv);
    player pl = {0};
    player_spawn(&pl, &t.w, EDGE - 0.3, 0.5);
    pl.pos.x = EDGE - PLAYER_HALF_W;
    nc_keypair kh = {0}, kp[2] = {{0}};
    nc_keypair_new(&kh);
    net *n = net_offline_host(&kh, "Host");
    for (int i = 0; i < 2; i++) {
        nc_keypair_new(&kp[i]);
        net_test_add_player(n, i + 1, i ? "B" : "A", kp[i].pk);
    }
    mp_world ctx = {&t.w, &t.ph, th, fx, &pl, hl, &inv, 1, NULL, NULL, NULL, NULL};
    mp *m = mp_create(n, &ctx);
    mp_poll(m, 0.0);
    const player_input idle = {0};
    for (int i = 0; i < 30; i++) mp_host_step(m, &idle);
    CHECK(mp_targets(m).n == 3);
    mp_avatar a[NET_MAX_PLAYERS];
    int na = mp_avatars(m, 0.0, 1.0, a, NET_MAX_PLAYERS);
    CHECK(na == 2);
    for (int i = 0; i < na && i < NET_MAX_PLAYERS; i++)
        CHECK(a[i].pos.x > EDGE - CHUNK_W && a[i].pos.x <= EDGE - PLAYER_HALF_W && a[i].pos.y == GROUND);
    mp_destroy(m);
    /* Spawning anywhere past the edge lands in its last block. */
    player_spawn(&pl, &t.w, EDGE + 7.5, -EDGE - 1e9);
    CHECK(pl.pos.x == EDGE - 0.5 && pl.pos.z == -EDGE + 0.5);
    mem_free(hl);
    mem_free(fx);
    thermo_destroy(th);
    tw_free(&t);
}

/* A host's heat field by a player at one corner, while it steps for a
 * player at the opposite one: 4e9 blocks apart, past an int (the
 * sanitizer build catches the overflow), so the cells are dropped as far. */
static void test_edge_thermo(void)
{
    test_world t = {0};
    tw_init(&t);
    world *w = &t.w;
    const int far[2] = {-EDGE_COLS, -EDGE_COLS};
    world_set_anchors(w, far, 1, 2);
    stream(w, EDGE - 8.5, EDGE - 8.5);
    thermo *th = thermo_create(3);
    const int at = -EDGE + 5; /* x and z */
    CHECK(world_set(w, at, GROUND, at, B_ICE, 0));
    thermo_on_block_changed(th, w, at, GROUND, at, B_AIR, B_ICE);
    CHECK(thermo_cell_count(th) > 0);
    for (int i = 0; i < 120; i++) thermo_step(th, w, 1.0 / 60.0, dv3(EDGE - 8.5, GROUND, EDGE - 8.5));
    CHECK(thermo_cell_count(th) == 0);
    thermo_destroy(th);
    world_set_anchors(w, NULL, 0, 2);
    tw_free(&t);
}

void test_edge_all(void)
{
    test_exact_noise();
    test_edge_terrain();
    test_edge_world();
    test_edge_mesh();
    test_edge_physics();
    test_edge_records();
    test_edge_save();
    test_edge_edits();
    test_edge_host();
    test_edge_thermo();
}
