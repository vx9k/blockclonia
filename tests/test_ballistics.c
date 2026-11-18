/* The air and bullets in flight: the standard atmosphere, the drag
 * tables, and trajectories checked against closed-form results (a
 * vacuum parabola, exponential decay where drag is constant, and the
 * classical lag rule for wind drift). */
#include "test_util.h"

#include "atmos.h"
#include "ballistics.h"
#include "combat.h"
#include "mem.h"

#include <math.h>

static void test_atmos(void)
{
    /* The standard atmosphere at sea level, 15 C. */
    atmos_air a = atmos_at(SEA_LEVEL, 15.0f);
    CHECK(fabs(a.rho - 1.225) < 0.002 && fabs(a.sound - 340.3) < 0.3 && a.gravity == GRAVITY);
    /* 2000 m up at 2 C: 79.5 kPa, 1.007 kg/m^3. */
    atmos_air hi = atmos_at(SEA_LEVEL + 2000.0, 2.0f);
    CHECK(fabs(hi.rho - 1.0066) < 0.003);
    /* Cold air is denser and carries sound slower; garbage is standard. */
    atmos_air cold = atmos_at(SEA_LEVEL, -20.0f);
    CHECK(cold.rho > a.rho * 1.13 && cold.sound < a.sound);
    atmos_air bad = atmos_at(SEA_LEVEL, NAN);
    CHECK(fabs(bad.rho - a.rho) < 1e-9);

    /* The wind is the same for the same seed and time, stays under 9 m/s
     * with a 35% gust, is level, and changes over time. */
    double most = 0.0, moved = 0.0;
    dvec3 prev = atmos_wind(7, 0.0);
    for (int i = 0; i < 2000; i++) {
        double t = i * 37.0;
        dvec3 w = atmos_wind(7, t), again = atmos_wind(7, t);
        CHECK(w.x == again.x && w.z == again.z && w.y == 0.0);
        most = fmax(most, hypot(w.x, w.z));
        moved += hypot(w.x - prev.x, w.z - prev.z);
        prev = w;
    }
    CHECK(most <= 9.0 * 1.35 + 1e-9 && most > 4.0 && moved > 50.0);
    dvec3 other = atmos_wind(8, 1000.0), mine = atmos_wind(7, 1000.0);
    CHECK(other.x != mine.x);

    /* No wind under the ground or where nothing is loaded. */
    test_world t = {0};
    tw_init(&t);
    dvec3 under = atmos_wind_at(&t.w, 7, 1000.0, dv3(4.5, GROUND - 3.0, 4.5));
    dvec3 open = atmos_wind_at(&t.w, 7, 1000.0, dv3(4.5, GROUND + 2.0, 4.5));
    dvec3 far = atmos_wind_at(&t.w, 7, 1000.0, dv3(1e6, GROUND + 2.0, 4.5));
    CHECK(under.x == 0.0 && under.z == 0.0 && far.x == 0.0 && open.x == mine.x && open.z == mine.z);
    tw_free(&t);
}

static void test_drag_table(void)
{
    /* Rows are exact, between rows linear, and the ends hold. */
    CHECK(fabs(ballistics_cd(DRAG_G7, 1.0) - 0.3803) < 1e-9);
    CHECK(fabs(ballistics_cd(DRAG_G1, 2.0) - 0.5934) < 1e-9);
    CHECK(fabs(ballistics_cd(DRAG_G7, 0.625) - 0.5 * (0.1194 + 0.1197)) < 1e-9);
    CHECK(fabs(ballistics_cd(DRAG_G1, -1.0) - 0.2629) < 1e-9 &&
          fabs(ballistics_cd(DRAG_G1, (double)NAN) - 0.2629) < 1e-9);
    CHECK(fabs(ballistics_cd(DRAG_G7, 9.0) - 0.1618) < 1e-9);
    /* The sound barrier: drag rises steeply through Mach 1. */
    CHECK(ballistics_cd(DRAG_G7, 1.05) > 3.0 * ballistics_cd(DRAG_G7, 0.85));
    CHECK(ammo_get(-1) == NULL && ammo_get(AMMO_COUNT) == NULL && ammo_get(AMMO_MATCH));
}

/* Flies round `kind` from x = 0 along +x at v0 until it passes `range`;
 * returns the time, with the state at that moment. */
static double fly(int kind, const atmos_air *air, double v0, double range, dvec3 *pos, dvec3 *vel)
{
    const ammo_def *a = ammo_get(kind);
    *pos = dv3(0.0, 0.0, 0.0);
    *vel = dv3(v0, 0.0, 0.0);
    double t = 0.0, dt = 1.0 / 240.0;
    while (pos->x < range && t < 30.0) {
        dvec3 p0 = *pos, v0s = *vel;
        ballistics_step(a, air, pos, vel, dt);
        if (pos->x >= range) {
            /* Back up to exactly `range` along the last step. */
            double f = (range - p0.x) / (pos->x - p0.x);
            *pos = dv3(range, p0.y + (pos->y - p0.y) * f, p0.z + (pos->z - p0.z) * f);
            *vel = dv3(v0s.x + (vel->x - v0s.x) * f, v0s.y + (vel->y - v0s.y) * f, v0s.z + (vel->z - v0s.z) * f);
            return t + dt * f;
        }
        t += dt;
    }
    return t;
}

static void test_trajectories(void)
{
    dvec3 pos = {0}, vel = {0};

    /* In a vacuum, the parabola: drop = g t^2 / 2 and no speed lost. */
    atmos_air vac = atmos_at(SEA_LEVEL, 15.0f);
    vac.rho = 0.0;
    double t = fly(AMMO_SMOKELESS, &vac, 800.0, 400.0, &pos, &vel);
    CHECK(fabs(t - 0.5) < 1e-9 && fabs(pos.y + 0.5 * GRAVITY * t * t) < 1e-6 && fabs(vel.x - 800.0) < 1e-9);

    /* Without gravity, at Mach 0.5 where the G7 curve is flat, drag is
     * k v^2 and speed falls as v0 exp(-k x). */
    atmos_air air = atmos_at(SEA_LEVEL, 15.0f);
    air.gravity = 0.0;
    const ammo_def *m = ammo_get(AMMO_MATCH);
    double v0 = 0.5 * air.sound;
    double k = ballistics_drag(m, &air, v0) / (v0 * v0);
    fly(AMMO_MATCH, &air, v0, 200.0, &pos, &vel);
    CHECK(fabs(vel.x / (v0 * exp(-k * 200.0)) - 1.0) < 0.002);

    /* Didion's lag rule: a crosswind W drifts a bullet W (t - x / v0),
     * the time it lost to drag. */
    air.wind = dv3(0.0, 0.0, 5.0);
    t = fly(AMMO_SMOKELESS, &air, 840.0, 300.0, &pos, &vel);
    double lag = 5.0 * (t - 300.0 / 840.0);
    CHECK(lag > 0.05 && fabs(pos.z / lag - 1.0) < 0.02);

    /* Drag is real: the 7.62 mm ball keeps about 74% of its speed at
     * 300 m (2050 ft/s of 2750, as G7 0.200 tables give). The heavy
     * boat-tail keeps more of its own; the round ball
     * the least of all. */
    air = atmos_at(SEA_LEVEL, 15.0f);
    double keep[AMMO_COUNT] = {0};
    for (int i = 0; i < AMMO_COUNT; i++) {
        fly(i, &air, (double)ammo_get(i)->muzzle, 100.0, &pos, &vel);
        keep[i] = dv3_len(vel) / (double)ammo_get(i)->muzzle;
    }
    fly(AMMO_SMOKELESS, &air, 840.0, 300.0, &pos, &vel);
    CHECK(vel.x > 0.72 * 840.0 && vel.x < 0.77 * 840.0);
    CHECK(keep[AMMO_MATCH] > keep[AMMO_SMOKELESS] && keep[AMMO_SMOKELESS] > keep[AMMO_ROUND_BALL]);
    CHECK(keep[AMMO_BLACK_LEAD] > keep[AMMO_ROUND_BALL] && keep[AMMO_ROUND_BALL] < 0.85);

    /* Thin, warm air up a mountain slows it less than cold air in a
     * valley. */
    atmos_air up = atmos_at(SEA_LEVEL + 300.0, 25.0f), down = atmos_at(SEA_LEVEL, -15.0f);
    dvec3 vu = {0}, vd = {0};
    fly(AMMO_BLACK_LEAD, &up, 400.0, 200.0, &pos, &vu);
    fly(AMMO_BLACK_LEAD, &down, 400.0, 200.0, &pos, &vd);
    CHECK(vu.x > vd.x + 2.0);
}

/* A shot through combat drifts with the wind it flies through. */
static void test_shot_drift(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 3);
    combat c = {0};
    combat_init(&c, 3);
    player pl = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);
    health *h = new_body(3);
    player *tp[1] = {&pl};
    health *th[1] = {h};
    combat_targets tg = {tp, th, 1};
    /* Find a moment with a good crosswind across a shot fired east. */
    while (fabs(atmos_wind(c.wind_seed, c.time).z) < 3.0) c.time += 60.0;
    double wz = atmos_wind(c.wind_seed, c.time).z;
    c.b[0] = (bullet){dv3(-44.5, GROUND + 3.0, 8.5), dv3(400.0, 0.0, 0.0), 4.0f, 0, AMMO_BLACK_LEAD};
    c.count = 1;
    for (int i = 0; i < 10 && c.count; i++) combat_step(&c, &ms, &t.ph, &tg);
    CHECK(c.count == 1 && c.b[0].pos.x > 15.0);
    CHECK((c.b[0].pos.z - 8.5) * wz > 0.0 && c.b[0].vel.x < 400.0);
    /* It flies on until the loaded world ends, then stops there rather than
     * crossing terrain that isn't loaded. */
    int steps = 10;
    for (; steps < 600 && c.count; steps++) combat_step(&c, &ms, &t.ph, &tg);
    CHECK(c.count == 0 && steps < 20);
    mem_free(h);
    tw_free(&t);
}

/* Rows of the published BRL tables, to catch a mistyped value. */
static void test_drag_rows(void)
{
    /* clang-format off */
    static const double G1_ROWS[][2] = {{0.0, 0.2629},  {0.5, 0.2032}, {0.8, 0.2546}, {0.9, 0.3415},
                                        {0.95, 0.4084}, {1.1, 0.5883}, {1.4, 0.6625}, {1.6, 0.6474},
                                        {2.5, 0.5397},  {3.0, 0.5133}, {4.0, 0.5006}, {5.0, 0.4988}};
    static const double G7_ROWS[][2] = {{0.0, 0.1198},   {0.5, 0.1194},  {0.8, 0.1242}, {0.9, 0.1464},
                                        {0.975, 0.2993}, {1.05, 0.4043}, {1.4, 0.3580}, {1.5, 0.3440},
                                        {2.5, 0.2697},   {3.0, 0.2424},  {4.0, 0.1935}, {5.0, 0.1618}};
    /* clang-format on */
    for (int i = 0; i < 12; i++) {
        CHECK(fabs(ballistics_cd(DRAG_G1, G1_ROWS[i][0]) - G1_ROWS[i][1]) < 1e-9);
        CHECK(fabs(ballistics_cd(DRAG_G7, G7_ROWS[i][0]) - G7_ROWS[i][1]) < 1e-9);
    }
    /* Positive everywhere, and a G7 boat-tail always drags less than a G1
     * flat base. */
    int ok = 1;
    for (int s = 0; s <= 550; s++) {
        double m = s * 0.01;
        double g1 = ballistics_cd(DRAG_G1, m), g7 = ballistics_cd(DRAG_G7, m);
        ok &= g1 > 0.1 && g7 > 0.1 && g7 < g1;
    }
    CHECK(ok);

    /* Each round is plausible: its form factor, sectional density over
     * ballistic coefficient, is near 1 against its own curve (0.4 to 1.6),
     * and its muzzle velocity is a rifle's. */
    for (int i = 0; i < AMMO_COUNT; i++) {
        const ammo_def *a = ammo_get(i);
        double d_in = (double)a->calibre / 0.0254, m_lb = (double)a->mass / 0.45359237;
        double form = m_lb / (d_in * d_in) / (double)a->bc;
        CHECK(a->name && a->name[0] && form > 0.4 && form < 1.6);
        CHECK(a->muzzle > 300.0f && a->muzzle < 1000.0f && a->drag <= DRAG_G7 && a->build <= BUILD_AP);
    }
}

static void test_air_edges(void)
{
    /* A bullet let fall settles at the speed where drag equals gravity. */
    atmos_air air = atmos_at(SEA_LEVEL, 15.0f);
    const ammo_def *a = ammo_get(AMMO_BLACK_LEAD);
    dvec3 pos = {0}, vel = {0};
    for (int i = 0; i < 60 * 120; i++) ballistics_step(a, &air, &pos, &vel, 1.0 / 60.0);
    double vt = -vel.y;
    CHECK(vt > 20.0 && fabs(ballistics_drag(a, &air, vt) / GRAVITY - 1.0) < 0.01);
    CHECK(fabs(vel.x) < 1e-9 && fabs(vel.z) < 1e-9);

    /* A tailwind keeps speed over the ground; a headwind takes it. */
    atmos_air tail = air, head = air;
    tail.wind = dv3(8.0, 0.0, 0.0);
    head.wind = dv3(-8.0, 0.0, 0.0);
    dvec3 vtail = {0}, vh = {0};
    fly(AMMO_BLACK_LEAD, &tail, 400.0, 200.0, &pos, &vtail);
    fly(AMMO_BLACK_LEAD, &head, 400.0, 200.0, &pos, &vh);
    CHECK(vtail.x > vh.x + 1.0);

    /* Nonsense in, finite air out. */
    const float temps[] = {-1000.0f, 1e9f, INFINITY, -INFINITY};
    const double heights[] = {-1e9, 1e9, 0.0};
    int finite = 1;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++) {
            atmos_air x = atmos_at(heights[j], temps[i]);
            finite &= isfinite(x.rho) && x.rho > 0.0 && isfinite(x.sound) && x.sound > 0.0;
        }
    CHECK(finite);
}

/* Fires one bullet of `speed` at a boar 10 m east; the hit points it took. */
static float shot_damage(test_world *t, double speed)
{
    mobs ms = {0};
    mobs_init(&ms, 4);
    int i = mobs_add(&ms, MOB_BOAR, dv3(18.5, GROUND, 8.5));
    if (i < 0) return -1.0f;
    ms.m[i].hp = 1000.0f;
    combat c = {0};
    combat_init(&c, 4);
    combat_targets tg = {NULL, NULL, 0};
    c.b[0] = (bullet){dv3(8.5, GROUND + 0.6, 8.5), dv3(speed, 0.0, 0.0), 4.0f, 0, AMMO_BLACK_LEAD};
    c.count = 1;
    int hits = 0;
    for (int k = 0; k < 10 && c.count; k++) hits += combat_step(&c, &ms, &t->ph, &tg);
    float took = 1000.0f - ms.m[i].hp;
    return hits == 1 ? took : -1.0f;
}

/* Lobs a bullet at a 6-high glass wall 21.5 m east, recording its x and
 * y each step; returns the steps it flew, with the panes broken. */
static int lob(test_world *t, double path[2][120], int *broke)
{
    mobs ms = {0};
    mobs_init(&ms, 5);
    combat c = {0};
    combat_init(&c, 5);
    combat_targets tg = {NULL, NULL, 0};
    for (int y = GROUND; y < GROUND + 6; y++) world_set(&t->w, 30, y, 8, B_GLASS, 0);
    c.b[0] = (bullet){dv3(8.5, GROUND + 1.0, 8.5), dv3(60.0, 14.0, 0.0), 4.0f, 0, AMMO_BLACK_LEAD};
    c.count = 1;
    int k = 0;
    for (; k < 120 && c.count; k++) {
        combat_step(&c, &ms, &t->ph, &tg);
        path[0][k] = c.count ? c.b[0].pos.x : 0.0;
        path[1][k] = c.count ? c.b[0].pos.y : 0.0;
    }
    *broke = 0;
    for (int y = GROUND; y < GROUND + 6; y++) *broke += world_get(&t->w, 30, y, 8) == B_AIR;
    return k;
}

static void test_shot_energy(void)
{
    test_world t = {0};
    tw_init(&t);
    /* Damage is energy: half the speed, a quarter of the harm. */
    float fast = shot_damage(&t, 400.0), slow = shot_damage(&t, 200.0);
    CHECK(fast > 10.0f && slow > 0.0f && fabsf(fast / slow - 4.0f) < 0.3f);

    /* A lobbed shot follows its arc and breaks one pane of a glass wall
     * where it comes down, the same way twice. */
    double first[2][120] = {{0}}, second[2][120] = {{0}};
    int broke[2] = {0, 0}, n[2] = {0, 0};
    for (int run = 0; run < 2; run++) {
        n[run] = lob(&t, run ? second : first, &broke[run]);
        for (int y = GROUND; y < GROUND + 6; y++) world_set(&t.w, 30, y, 8, B_AIR, 0);
    }
    int same = n[0] == n[1];
    for (int k = 0; same && k < n[0]; k++) same = first[0][k] == second[0][k] && first[1][k] == second[1][k];
    CHECK(n[0] > 0 && n[0] < 120 && broke[0] == 1 && broke[1] == 1 && same);
    tw_free(&t);
}

/* Range comes from the flight, not a timer. */
static void test_shot_range(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 7);
    combat c = {0};
    combat_init(&c, 7);
    combat_targets tg = {NULL, NULL, 0};
    /* Straight up at 60 m/s: still climbing or falling well past the old
     * 4 s limit, and back on the ground in about 10 s. */
    c.b[0] = (bullet){dv3(0.5, GROUND + 1.0, 0.5), dv3(0.0, 60.0, 0.0), 60.0f, 0, AMMO_BLACK_LEAD};
    c.count = 1;
    int steps = 0;
    double top = 0.0;
    for (; steps < 60 * 60 && c.count; steps++) {
        combat_step(&c, &ms, &t.ph, &tg);
        if (c.count) top = fmax(top, c.b[0].pos.y);
        if (steps == 5 * 60) CHECK(c.count == 1);
    }
    CHECK(c.count == 0 && steps > 6 * 60 && steps < 30 * 60);
    /* Drag takes height: less than the vacuum's v²/2g = 183 m. */
    CHECK(top - (GROUND + 1.0) > 80.0 && top - (GROUND + 1.0) < 183.0);

    /* A shot already outside the loaded world ends at once. */
    c.b[0] = (bullet){dv3(5000.5, GROUND + 3.0, 0.5), dv3(-400.0, 0.0, 0.0), 60.0f, 0, AMMO_BLACK_LEAD};
    c.count = 1;
    CHECK(combat_step(&c, &ms, &t.ph, &tg) == 0 && c.count == 0);

    /* With every slot in flight, a trigger pull still fires: the shot fired
     * longest ago gives way. */
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, I_RIFLE, 1);
    inv_add(&inv, I_CARTRIDGE, 1);
    for (int i = 0; i < COMBAT_SHOTS; i++)
        c.b[i] = (bullet){dv3(0.5, GROUND + 50.0, 0.5), dv3(0.0, 50.0, 0.0), 20.0f + (float)i, 0, AMMO_BLACK_LEAD};
    c.b[7].life = 2.0f;
    c.count = COMBAT_SHOTS;
    combat_out o = combat_frame(&c, &ms, &t.ph, &tg, 0, &inv, dv3(0.5, GROUND + 1.6, 0.5), v3(1.0f, 0.0f, 0.0f), 5.0, 1,
                                1.0f, 1.0f / 60.0f);
    int old = 0, fresh = 0;
    for (int i = 0; i < c.count; i++) {
        old += c.b[i].life < 3.0f;
        fresh += c.b[i].life > 59.0f;
    }
    CHECK(o.fired && c.count == COMBAT_SHOTS && old == 0 && fresh == 1 && inv_count(&inv, I_CARTRIDGE) == 0);
    tw_free(&t);
}

void test_ballistics_all(void)
{
    test_atmos();
    test_drag_table();
    test_drag_rows();
    test_air_edges();
    test_trajectories();
    test_shot_drift();
    test_shot_energy();
    test_shot_range();
}
