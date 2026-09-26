/* Falling blocks: bouncing, shattering, tumbling and pinning the player
 * (physics.c). */
#include "test_util.h"

#include <math.h>
#include <string.h>

/* One body at rest, its min corner at (x, y, z). */
static body *drop(test_world *t, double x, double y, double z, uint8_t id)
{
    body *b = &t->ph.bodies[t->ph.body_count++];
    *b = (body){.pos = dv3(x, y, z), .prev_pos = dv3(x, y, z), .block = id};
    b->rot[3] = 1.0f;
    return b;
}

static int count_blocks(const world *w, int x0, int z0, int r, int y, uint8_t id)
{
    int n = 0;
    for (int z = z0 - r; z <= z0 + r; z++)
        for (int x = x0 - r; x <= x0 + r; x++) n += world_get(w, x, y, z) == id;
    return n;
}

static void spawn_away(test_world *t, player *p) { player_spawn(p, &t->w, 20.5, 20.5); }

/* Glass landing at 9.9 m/s (5 m, sqrt(2gh)) is past the 5 m/s it survives:
 * it leaves no block, only a shatter event. From 0.5 m (3.1 m/s) it lands
 * whole. */
static void test_shatter(void)
{
    test_world t;
    tw_init(&t);
    player p;
    spawn_away(&t, &p);
    player_input idle = {0};
    drop(&t, 2, GROUND + 5, 2, B_GLASS);
    int shattered = 0;
    ipos at = {0, 0, 0};
    uint8_t what = 0;
    for (int i = 0; i < 180 && t.ph.body_count; i++) {
        physics_step(&t.ph, &p, &idle);
        if (t.ph.shatter_count) {
            shattered += t.ph.shatter_count;
            at = t.ph.shatter_pos[0];
            what = t.ph.shatter_block[0];
        }
    }
    CHECK(t.ph.body_count == 0);
    CHECK(shattered == 1 && what == B_GLASS);
    CHECK(at.x == 2 && at.y == GROUND && at.z == 2);
    CHECK(count_blocks(&t.w, 2, 2, 2, GROUND, B_GLASS) == 0);
    CHECK(count_blocks(&t.w, 2, 2, 2, GROUND + 1, B_GLASS) == 0);
    /* The ring is per step: the next step starts empty. */
    physics_step(&t.ph, &p, &idle);
    CHECK(t.ph.shatter_count == 0);

    drop(&t, 8, GROUND + 0.5, 8, B_GLASS);
    shattered = 0;
    for (int i = 0; i < 240 && t.ph.body_count; i++) {
        physics_step(&t.ph, &p, &idle);
        shattered += t.ph.shatter_count;
    }
    CHECK(shattered == 0 && t.ph.body_count == 0);
    CHECK(count_blocks(&t.w, 8, 8, 1, GROUND, B_GLASS) == 1);
    tw_free(&t);
}

/* Planks (e = 0.35) from 5 m land at ~9.9 m/s and rebound at ~3.46 m/s,
 * rising (v e)^2 / 2g = 0.61 m; the second landing at ~3.46 m/s gives a
 * 1.2 m/s hop; the third is too soft, and the block sets in the grid. */
static void test_bounce(void)
{
    test_world t;
    tw_init(&t);
    player p;
    spawn_away(&t, &p);
    player_input idle = {0};
    drop(&t, 2, GROUND + 5, 2, B_PLANKS);
    int max_bounces = 0;
    double first_up = 0.0, peak = 0.0;
    for (int i = 0; i < 300 && t.ph.body_count; i++) {
        physics_step(&t.ph, &p, &idle);
        if (!t.ph.body_count) break;
        const body *b = &t.ph.bodies[0];
        if (b->bounces == 1 && max_bounces == 0) first_up = b->vel.y;
        if (b->bounces > max_bounces) max_bounces = b->bounces;
        if (b->bounces == 1 && b->pos.y - GROUND > peak) peak = b->pos.y - GROUND;
    }
    CHECK(max_bounces == 2);
    /* Drag on a 500 kg/m^3 cube costs well under 1 % over 5 m. */
    CHECK(fabs(first_up - 0.35 * sqrt(2.0 * GRAVITY * 5.0)) < 0.1);
    /* Discrete 60 Hz steps sample the apex within a few mm. */
    CHECK(peak > 0.55 && peak < 0.65);
    CHECK(t.ph.body_count == 0);
    /* The scatter is at most 20 % of each rebound: it cannot carry the
     * block further than the next cell. */
    CHECK(count_blocks(&t.w, 2, 2, 1, GROUND, B_PLANKS) == 1);
    CHECK(count_blocks(&t.w, 2, 2, 1, GROUND + 1, B_PLANKS) == 0);

    /* Stone from 0.5 m: 3.1 m/s * 0.25 = 0.8 m/s would be a 3 cm hop,
     * below the 1 m/s floor, so it sets where it fell. */
    drop(&t, 6, GROUND + 0.5, 6, B_STONE);
    int bounced = 0;
    for (int i = 0; i < 120 && t.ph.body_count; i++) {
        physics_step(&t.ph, &p, &idle);
        if (t.ph.body_count && t.ph.bodies[0].bounces) bounced = 1;
    }
    CHECK(!bounced && t.ph.body_count == 0);
    CHECK(world_get(&t.w, 6, GROUND, 6) == B_STONE);
    tw_free(&t);
}

static float quat_len(const float q[4]) { return sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); }

static float spin_rate(const body *b)
{
    return sqrtf(b->spin[0] * b->spin[0] + b->spin[1] * b->spin[1] + b->spin[2] * b->spin[2]);
}

/* A stone arm along +X whose pillar is knocked out: every block tumbles,
 * rolling outward (spin about -Z for +X), faster further out, and the
 * drawn orientation stays a unit quaternion while it turns. */
static void test_tumble(void)
{
    test_world t;
    tw_init(&t);
    physics *ph = &t.ph;
    int top = GROUND + 5;
    for (int y = GROUND; y <= top; y++) world_set(&t.w, 0, y, 0, B_STONE, 0);
    for (int i = 1; i <= 7; i++) world_set(&t.w, i, top, 0, B_STONE, 0);
    ph->struct_count = 0;
    world_set(&t.w, 0, GROUND + 2, 0, B_AIR, 0);
    ph->struct_count = 0;
    int fell = physics_check_structure(ph, 0, GROUND + 2, 0);
    CHECK(fell == 10);
    int all_spin = 1, outward = 1;
    float near_w = 0.0f, far_w = 0.0f;
    for (int i = 0; i < ph->body_count; i++) {
        const body *b = &ph->bodies[i];
        float w = spin_rate(b);
        all_spin &= w > 0.2f && w <= 3.0f;
        if (b->pos.x > 0.5) outward &= b->spin[2] < 0.0f && fabsf(b->spin[0]) < 1e-6f;
        if ((int)b->pos.x == 1) near_w = w;
        if ((int)b->pos.x == 7) far_w = w;
    }
    CHECK(all_spin && outward);
    CHECK(near_w >= 1.0f && far_w > near_w + 1.0f);

    player p;
    spawn_away(&t, &p);
    player_input idle = {0};
    float worst = 0.0f, min_w = 1.0f;
    for (int s = 0; s < 240 && ph->body_count; s++) {
        physics_step(ph, &p, &idle);
        for (int i = 0; i < ph->body_count; i++) {
            float e = fabsf(quat_len(ph->bodies[i].rot) - 1.0f);
            if (e > worst) worst = e;
            if (fabsf(ph->bodies[i].rot[3]) < min_w) min_w = fabsf(ph->bodies[i].rot[3]);
        }
    }
    CHECK(worst < 1e-5f);
    CHECK(min_w < 0.99f); /* it really turned: more than 16 degrees */
    CHECK(ph->body_count == 0);
    CHECK(world_get(&t.w, 3, GROUND, 0) == B_STONE); /* collisions stayed on the grid */
    tw_free(&t);
}

/* A log lowered onto a standing player's head rests there: its whole
 * 600 kg bears on the top of the head. Stone dropped from 2.2 m above the
 * head strikes at sqrt(2 g 2.2) = 6.6 m/s, cannot drive a standing player
 * into the ground, then lies there too. Stepping out from under frees it. */
static void test_pinned(void)
{
    test_world t;
    tw_init(&t);
    player p;
    player_spawn(&p, &t.w, 5.5, 5.5);
    CHECK(!p.flying);
    player_input idle = {0};
    drop(&t, 5, GROUND + PLAYER_HEIGHT + 0.01, 5, B_LOG);
    run_steps(&t, &p, &idle, 30);
    CHECK(t.ph.body_count == 1);
    CHECK(fabsf(t.ph.pinned_mass - 600.0f) < 1.0f);
    CHECK(fabsf(t.ph.pinned_height - 1.0f) < 0.01f);
    CHECK(t.ph.hit_count == 0);

    p.pos.x += 2.0;
    physics_step(&t.ph, &p, &idle);
    CHECK(t.ph.pinned_mass == 0.0f);
    run_steps(&t, &p, &idle, 120);
    CHECK(t.ph.body_count == 0 && world_get(&t.w, 5, GROUND, 5) == B_LOG);

    player_spawn(&p, &t.w, 9.5, 9.5);
    drop(&t, 9, GROUND + PLAYER_HEIGHT + 2.2, 9, B_STONE);
    double hit = 0.0;
    int stood = 1;
    for (int i = 0; i < 90; i++) {
        physics_step(&t.ph, &p, &idle);
        if (t.ph.hit_count) hit = (double)t.ph.hits[0].speed;
        stood &= fabs(p.pos.y - GROUND) < 1e-9 && fabs(p.vel.x) < 1e-9 && fabs(p.vel.z) < 1e-9;
    }
    CHECK(stood);
    CHECK(fabs(hit - sqrt(2.0 * GRAVITY * 2.2)) < 0.2);
    CHECK(fabsf(t.ph.pinned_mass - 2600.0f) < 1.0f);
    tw_free(&t);
}

/* Where a body pushed along the ground at v comes to rest (its min
 * corner's x), or -1 if it never set. */
static double slide_to(test_world *t, player *p, double x0, int z, double v)
{
    player_input idle = {0};
    body *b = drop(t, x0, GROUND, z, B_STONE);
    b->vel.x = v;
    double last = -1.0;
    for (int i = 0; i < 600 && t->ph.body_count; i++) {
        last = t->ph.bodies[0].pos.x;
        physics_step(&t->ph, p, &idle);
    }
    return t->ph.body_count ? -1.0 : last;
}

/* A block skidding at 2 m/s brakes at mu g on what it rests on and only
 * then sets: v^2 / (2 mu g) = 0.34 m on grass (mu 0.6), 6.8 m on ice
 * (mu 0.03). */
static void test_slide(void)
{
    test_world t;
    tw_init(&t);
    player p;
    spawn_away(&t, &p);
    player_input idle_input = {0};
    double grass = slide_to(&t, &p, 0.0, 2, 2.0);
    CHECK(fabs(grass - 4.0 / (2.0 * 0.6 * GRAVITY)) < 0.03);
    CHECK(world_get(&t.w, 0, GROUND, 2) == B_STONE);
    for (int x = -1; x <= 12; x++) world_set(&t.w, x, GROUND - 1, 6, B_ICE, 0);
    double ice = slide_to(&t, &p, 0.0, 6, 2.0);
    CHECK(fabs(ice - 4.0 / (2.0 * 0.03 * GRAVITY)) < 0.1);
    CHECK(world_get(&t.w, 7, GROUND, 6) == B_STONE); /* sets in the cell its centre stopped over */

    /* Resting with its centre 0.2 m past the edge of a one-block pit, it
     * pivots over and drops in instead of skating across. */
    world_set(&t.w, 4, GROUND - 1, 10, B_AIR, 0);
    drop(&t, 3.7, GROUND, 10, B_STONE);
    for (int i = 0; i < 240 && t.ph.body_count; i++) physics_step(&t.ph, &p, &idle_input);
    CHECK(t.ph.body_count == 0 && world_get(&t.w, 4, GROUND - 1, 10) == B_STONE);
    CHECK(world_get(&t.w, 3, GROUND, 10) == B_AIR && world_get(&t.w, 4, GROUND, 10) == B_AIR);
    tw_free(&t);
}

/* A 6-deep pool with its surface at GROUND. */
static void make_pool(test_world *t)
{
    for (int y = GROUND - 6; y < GROUND; y++)
        for (int z = -3; z <= 3; z++)
            for (int x = -3; x <= 3; x++) world_set(&t->w, x, y, z, B_WATER, 0);
    t->ph.fluid_next.count = 0; /* level pool: nothing to simulate */
    memset(t->ph.fluid_next.set, 0, t->ph.fluid_next.set_cap * sizeof(uint32_t));
}

/* Stone falling 4 m hits the water at sqrt(2 g 4) = 8.9 m/s: one splash at
 * the surface, over its centre. Lowered in from 10 cm (1.4 m/s) it makes
 * none. The player jumping in from 3 m (7.7 m/s) splashes too. */
static void test_splash(void)
{
    test_world t;
    tw_init(&t);
    make_pool(&t);
    player p;
    spawn_away(&t, &p);
    player_input idle = {0};
    drop(&t, 0, GROUND + 4, 0, B_STONE);
    int n = 0;
    dvec3 at = dv3(0, 0, 0);
    float v = 0.0f;
    for (int i = 0; i < 120; i++) {
        physics_step(&t.ph, &p, &idle);
        if (t.ph.splash_count) {
            n += t.ph.splash_count;
            at = t.ph.splash_pos[0];
            v = t.ph.splash_speed[0];
        }
    }
    CHECK(n == 1);
    /* The step that crosses the surface moves up to 15 cm past it; its
     * mean speed is within that step's gain (g dt = 0.16 m/s) of 8.86. */
    CHECK(fabs((double)v - sqrt(2.0 * GRAVITY * 4.0)) < 0.25);
    CHECK(fabs(at.x - 0.5) < 1e-9 && fabs(at.z - 0.5) < 1e-9 && fabs(at.y - GROUND) < 1e-6);

    t.ph.body_count = 0;
    drop(&t, 2, GROUND + 0.1, 2, B_STONE);
    n = 0;
    for (int i = 0; i < 60; i++) {
        physics_step(&t.ph, &p, &idle);
        n += t.ph.splash_count;
    }
    CHECK(n == 0);

    t.ph.body_count = 0;
    p.pos = p.prev_pos = dv3(-1.5, GROUND + 3.0, -1.5);
    p.vel = dv3(0, 0, 0);
    p.on_ground = 0;
    n = 0;
    for (int i = 0; i < 90; i++) {
        physics_step(&t.ph, &p, &idle);
        if (t.ph.splash_count) {
            n += t.ph.splash_count;
            v = t.ph.splash_speed[0];
            at = t.ph.splash_pos[0];
        }
    }
    CHECK(n == 1);
    CHECK(fabs((double)v - sqrt(2.0 * GRAVITY * 3.0)) < 0.3);
    CHECK(fabs(at.x + 1.5) < 1e-9 && fabs(at.y - GROUND) < 1e-6);
    tw_free(&t);
}

/* Blocks moving sideways into a standing player. Leaves (150 kg) at ~3 m/s
 * share momentum like a perfectly inelastic collision: the player gains
 * 150 v / 225 and the block keeps the same velocity, so 150 v is
 * conserved. Stone (2600 kg) at 8 m/s would give 7.8 m/s; the shove is
 * capped at 5 m/s, which grass (mu 0.6) brakes over v^2 / 2 mu g = 2.1 m. */
static void test_knockback(void)
{
    test_world t;
    tw_init(&t);
    player p;
    player_spawn(&p, &t.w, 5.5, 5.5);
    player_input idle = {0};
    body *b = drop(&t, 3.5, GROUND + 0.5, 5, B_LEAVES);
    b->vel.x = 3.0;
    double rel = 0.0;
    int i;
    for (i = 0; i < 60 && !t.ph.hit_count; i++) physics_step(&t.ph, &p, &idle);
    CHECK(t.ph.hit_count == 1);
    if (t.ph.hit_count) rel = (double)t.ph.hits[0].speed;
    CHECK(rel > 2.9 && rel < 3.0); /* air drag on leaves is small */
    CHECK(fabs(p.vel.x - 150.0 * rel / 225.0) < 1e-4);
    CHECK(t.ph.body_count == 1 && fabs(t.ph.bodies[0].vel.x - p.vel.x) < 1e-4);
    CHECK(fabs(150.0 * rel - (75.0 * p.vel.x + 150.0 * t.ph.bodies[0].vel.x)) < 1e-2);

    t.ph.body_count = 0;
    run_steps(&t, &p, &idle, 120);
    player_spawn(&p, &t.w, 5.5, 5.5);
    b = drop(&t, 3.0, GROUND + 0.4, 5, B_STONE);
    b->vel.x = 8.0;
    for (i = 0; i < 60 && !t.ph.hit_count; i++) physics_step(&t.ph, &p, &idle);
    CHECK(t.ph.hit_count == 1);
    CHECK(fabs(p.vel.x - 5.0) < 1e-9);
    double x0 = p.pos.x;
    run_steps(&t, &p, &idle, 120);
    CHECK(p.pos.x - x0 > 1.8 && p.pos.x - x0 < 2.4);
    CHECK(fabs(p.vel.x) < 1e-9);
    tw_free(&t);
}

void test_bodies_all(void)
{
    test_shatter();
    test_bounce();
    test_tumble();
    test_slide();
    test_pinned();
    test_splash();
    test_knockback();
}
