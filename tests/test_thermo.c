#include "thermo.h"
/* Thermodynamics: the day clock, ambient air, the heat field, campfires,
 * melting and freezing (thermo.c). */
#include "test_util.h"

#include <math.h>

/* World edits reach both physics and thermo, as in the game. */
typedef struct {
    physics *ph;
    thermo *th;
    const world *w;
} hooks;

static void both_hooks(void *user, int x, int y, int z, uint8_t old_id, uint8_t new_id)
{
    hooks *h = user;
    physics_on_block_changed(h->ph, x, y, z, old_id, new_id);
    thermo_on_block_changed(h->th, h->w, x, y, z, old_id, new_id);
}

static void attach(test_world *tw, hooks *h, thermo *th)
{
    h->ph = &tw->ph;
    h->th = th;
    h->w = &tw->w;
    tw->w.edit_user = h;
    tw->w.on_block_changed = both_hooks;
}

static void detach(test_world *tw)
{
    tw->w.edit_user = &tw->ph;
    tw->w.on_block_changed = physics_on_block_changed;
}

static void advance(thermo *th, world *w, double seconds, double dt, dvec3 eye)
{
    for (int i = 0, n = (int)(seconds / dt + 0.5); i < n; i++) thermo_step(th, w, dt, eye);
}

static float sky_sum(const thermo *th)
{
    float c[3];
    thermo_sky(th, c);
    return c[0] + c[1] + c[2];
}

static void test_sun(void)
{
    thermo *th = thermo_create(1);
    CHECK(fabs(thermo_day_time(th) - THERMO_DAY_START) < 1e-12);
    thermo_set_day_time(th, 0.5);
    float noon = thermo_daylight(th), sky_noon = sky_sum(th);
    float c[3];
    thermo_sky(th, c);
    CHECK(fabsf(c[0] - 0.53f) < 0.01f && fabsf(c[1] - 0.74f) < 0.01f && fabsf(c[2] - 1.0f) < 0.01f);
    thermo_set_day_time(th, 0.25);
    float dawn = thermo_daylight(th);
    thermo_sky(th, c);
    CHECK(c[0] > c[2]); /* sunrise glows orange */
    thermo_set_day_time(th, 0.0);
    float midnight = thermo_daylight(th), sky_night = sky_sum(th);
    CHECK(fabsf(noon - 1.0f) < 1e-3f);
    CHECK(fabsf(midnight - 0.18f) < 1e-3f);
    CHECK(noon > dawn && dawn > midnight);
    CHECK(sky_night < 0.2f * sky_noon);
    /* Smooth through dawn and dusk: no jumps between 1-minute samples. */
    float prev = thermo_daylight(th), jump = 0.0f;
    for (int i = 1; i <= 1440; i++) {
        thermo_set_day_time(th, i / 1440.0);
        float d = thermo_daylight(th);
        jump = fmaxf(jump, fabsf(d - prev));
        prev = d;
    }
    CHECK(jump < 0.01f);
    thermo_set_day_time(th, 1.75);
    CHECK(fabs(thermo_day_time(th) - 0.75) < 1e-12);

    /* One day is 20 real minutes: 600 s is half a day. */
    test_world tw;
    tw_init(&tw);
    thermo_set_day_time(th, 0.25);
    advance(th, &tw.w, 600.0, 1.0, dv3(0.5, GROUND, 0.5));
    CHECK(fabs(thermo_day_time(th) - 0.75) < 1e-9);
    tw_free(&tw);
    thermo_destroy(th);
}

static void test_ambient(void)
{
    test_world tw;
    tw_init(&tw);
    thermo *th = thermo_create(2);
    world *w = &tw.w;
    thermo_set_day_time(th, 0.375); /* 09:00: the diurnal swing crosses zero */
    CHECK(fabsf(thermo_ambient(th, w, 0.5, 92.0, 0.5)) < 0.1f);
    CHECK(fabsf(thermo_ambient(th, w, 0.5, SEA_LEVEL, 0.5) - 14.0f) < 0.1f);
    CHECK(thermo_ambient(th, w, 0.5, 70.0, 0.5) > thermo_ambient(th, w, 0.5, 110.0, 0.5));
    thermo_set_day_time(th, 15.0 / 24.0);
    float warm = thermo_ambient(th, w, 0.5, SEA_LEVEL, 0.5);
    thermo_set_day_time(th, 3.0 / 24.0);
    float cold = thermo_ambient(th, w, 0.5, SEA_LEVEL, 0.5);
    CHECK(fabsf(warm - cold - 10.0f) < 0.1f); /* +/-5 C, warmest mid-afternoon */
    thermo_set_day_time(th, 0.5);
    CHECK(thermo_ambient(th, w, 0.5, SEA_LEVEL, 0.5) < warm); /* lags the sun */

    /* Under a roof the swing is damped and the air sits near 10 C. */
    world_set(w, 5, GROUND + 4, 5, B_STONE, 0);
    thermo_set_day_time(th, 15.0 / 24.0);
    float in_warm = thermo_ambient(th, w, 5.5, GROUND + 0.5, 5.5);
    thermo_set_day_time(th, 3.0 / 24.0);
    float in_cold = thermo_ambient(th, w, 5.5, GROUND + 0.5, 5.5);
    CHECK(in_warm - in_cold < 0.3f * (warm - cold));
    CHECK(fabsf(0.5f * (in_warm + in_cold) - 10.0f) < 3.0f);
    thermo_destroy(th);
    tw_free(&tw);
}

static void test_fire_air_and_radiation(void)
{
    test_world tw;
    tw_init(&tw);
    thermo *th = thermo_create(3);
    hooks h;
    attach(&tw, &h, th);
    world *w = &tw.w;
    dvec3 eye = dv3(4.5, GROUND, 0.5);
    thermo_set_day_time(th, 0.375);
    world_set(w, 0, GROUND, 0, B_CAMPFIRE, 0);
    CHECK(thermo_fire_count(th) == 1);
    CHECK(fabsf(thermo_block_temp(th, w, 0, GROUND, 0) - 827.0f) < 30.0f);

    /* The plume: the air cell's time constant is ~10 s of survival time, so
     * 5 real s (360 s) reaches steady state; Heskestad's centreline rise
     * 1-2 m above a 10 kW fire is ~100 and ~30 K, a cell average less. */
    advance(th, w, 5.0, 1.0 / 60.0, eye);
    float amb = thermo_ambient(th, w, 0.5, GROUND + 1.5, 0.5);
    float above1 = thermo_air(th, w, 0.5, GROUND + 1.5, 0.5) - amb;
    float above2 = thermo_air(th, w, 0.5, GROUND + 2.5, 0.5) - thermo_ambient(th, w, 0.5, GROUND + 2.5, 0.5);
    float side2 = thermo_air(th, w, 2.5, GROUND + 0.5, 0.5) - thermo_ambient(th, w, 2.5, GROUND + 0.5, 0.5);
    CHECK(above1 > 20.0f && above1 < 150.0f);
    CHECK(above2 > 8.0f && above2 < above1);
    CHECK(side2 >= 0.0f && side2 < 0.5f * above2); /* hot air rises */
    CHECK(thermo_cell_count(th) > 50 && thermo_cell_count(th) < 2000);

    /* Radiant flux: ~296 W/m^2 at 1 m, a quarter of that at 2 m. */
    double q1 = thermo_radiant(th, w, dv3(1.5, GROUND + 0.5, 0.5));
    double q2 = thermo_radiant(th, w, dv3(2.5, GROUND + 0.5, 0.5));
    CHECK(q1 > 270.0 && q1 < 320.0);
    CHECK(fabs(q1 / q2 - 4.0) < 0.05);
    CHECK(thermo_radiant(th, w, dv3(0.5, GROUND + 0.5, 13.5)) == 0.0); /* out of reach */
    world_set(w, 0, GROUND, -1, B_STONE, 0);
    CHECK(thermo_radiant(th, w, dv3(0.5, GROUND + 0.5, -1.5)) == 0.0); /* behind a wall */
    CHECK(thermo_near_fire(th, w, dv3(1.5, GROUND + 0.5, 0.5), 1.5));
    CHECK(!thermo_near_fire(th, w, dv3(3.5, GROUND + 0.5, 0.5), 1.5));

    /* Put out by water beside it: ash, no more heat, and the field fades. */
    world_set(w, 1, GROUND, 0, B_WATER, 0);
    thermo_step(th, w, 1.0 / 60.0, eye);
    CHECK(world_get(w, 0, GROUND, 0) == B_ASH);
    CHECK(thermo_fire_count(th) == 0);
    CHECK(thermo_radiant(th, w, dv3(0.5, GROUND + 0.5, 1.5)) == 0.0);
    advance(th, w, 120.0, 0.5, eye);
    float after = thermo_air(th, w, 0.5, GROUND + 1.5, 0.5) - thermo_ambient(th, w, 0.5, GROUND + 1.5, 0.5);
    CHECK(after < 3.0f);
    detach(&tw);
    thermo_destroy(th);
    tw_free(&tw);
}

static void test_fuel(void)
{
    test_world tw;
    tw_init(&tw);
    thermo *th = thermo_create(4);
    hooks h;
    attach(&tw, &h, th);
    world *w = &tw.w;
    dvec3 eye = dv3(0.5, GROUND, 0.5);

    /* One unit burns for a survival-clock hour: 3600 / 72 = 50 real s. */
    world_set(w, 0, GROUND, 0, B_CAMPFIRE, 1);
    advance(th, w, 49.0, 0.1, eye);
    CHECK(world_get(w, 0, GROUND, 0) == B_CAMPFIRE);
    advance(th, w, 2.0, 0.1, eye);
    CHECK(world_get(w, 0, GROUND, 0) == B_ASH);
    CHECK(thermo_fire_count(th) == 0);

    /* Two more units at 25 s: three in all, ash at 150 s. */
    world_set(w, 4, GROUND, 4, B_CAMPFIRE, 1);
    advance(th, w, 25.0, 0.1, eye);
    CHECK(thermo_add_fuel(th, w, 4, GROUND, 4, 2) == 1);
    CHECK(world_get_meta(w, 4, GROUND, 4) == 3);
    advance(th, w, 123.0, 0.1, eye);
    CHECK(world_get(w, 4, GROUND, 4) == B_CAMPFIRE);
    advance(th, w, 4.0, 0.1, eye);
    CHECK(world_get(w, 4, GROUND, 4) == B_ASH);

    /* Full fires and other blocks refuse fuel; a log tops a fire up to 8. */
    world_set(w, 8, GROUND, 8, B_CAMPFIRE, 0);
    CHECK(thermo_add_fuel(th, w, 8, GROUND, 8, 1) == 0);
    CHECK(thermo_add_fuel(th, w, 8, GROUND - 1, 8, 1) == 0);
    world_set(w, 8, GROUND, 8, B_CAMPFIRE, 6);
    CHECK(thermo_add_fuel(th, w, 8, GROUND, 8, 4) == 1);
    CHECK(world_get_meta(w, 8, GROUND, 8) == 0);
    CHECK(thermo_add_fuel(th, w, 8, GROUND, 8, 0) == 0);

    /* Campfires from a save (no change hook) are found by the scan. */
    detach(&tw);
    thermo *th2 = thermo_create(5);
    advance(th2, w, 1.0, 1.0 / 60.0, eye);
    CHECK(thermo_fire_count(th2) == 1);
    thermo_destroy(th2);
    thermo_destroy(th);
    tw_free(&tw);
}

/* Heat a face-conductance model would feed a melting block at 0 C: the
 * fire's radiation onto one face (a sixth of eps sigma (T_f^4 - T_s^4) A_f,
 * 90% absorbed), the fire's hot gas and the air on the other faces through
 * a film h = 10 W/(m^2 K) in series with half the block (k = 2.2 ice,
 * 0.15 snow), and the grass below (k = 0.8), all at the temperatures the
 * field settled to. */
static double melt_power(const thermo *th, const world *w, int x, int fx, double k)
{
    double tf2 = 1100.0 * 1100.0, ts2 = 306.0 * 306.0;
    double rad = 0.9 * 0.09 * 5.670374e-8 * (tf2 * tf2 - ts2 * ts2) * 0.5 / 6.0;
    double g_air = 1.0 / (0.1 + 0.5 / k), g_ground = 2.0 * k * 0.8 / (k + 0.8);
    double p = rad + g_ground * (double)thermo_block_temp(th, w, x, GROUND - 1, 0);
    p += g_air * (double)thermo_air(th, w, fx + 0.5, GROUND + 0.5, 0.5);
    p += g_air * (double)thermo_air(th, w, x + (x - fx) + 0.5, GROUND + 0.5, 0.5);
    p += g_air * (double)thermo_air(th, w, x + 0.5, GROUND + 1.5, 0.5);
    p += g_air * (double)thermo_air(th, w, x + 0.5, GROUND + 0.5, 1.5);
    p += g_air * (double)thermo_air(th, w, x + 0.5, GROUND + 0.5, -0.5);
    return p;
}

static void test_melting(void)
{
    test_world tw;
    tw_init(&tw);
    thermo *th = thermo_create(6);
    hooks h;
    attach(&tw, &h, th);
    world *w = &tw.w;
    dvec3 eye = dv3(0.5, GROUND, 0.5);
    thermo_set_day_time(th, 0.375);
    world_set(w, -10, GROUND, 0, B_CAMPFIRE, 0);
    world_set(w, -9, GROUND, 0, B_ICE, 0);
    world_set(w, 10, GROUND, 0, B_SNOW, 0);
    world_set(w, 11, GROUND, 0, B_CAMPFIRE, 0);
    /* In 14 C air the blocks join the field at their melting point. */
    CHECK(fabsf(thermo_block_temp(th, w, -9, GROUND, 0)) < 1e-4f);
    CHECK(thermo_cell_count(th) > 0);

    /* The estimate: latent heat over the power reaching the block once the
     * field is steady (10 min real). Sampled at 21:00, when the diurnal
     * swing is at its mean, as it is on average over the ~3 days of
     * survival time the melting takes. Ice: 917 kg * 334 kJ/kg = 306 MJ
     * over ~1.2 kW (0.56 radiation, ~0.4 gas of the fire, ~0.2 air) = 70 real
     * min. Snow insulates (k = 0.15): 134 MJ over ~0.6 kW, almost all
     * radiation = 50 real min. The fires are topped up every 5 minutes. */
    double t_ice = -1.0, t_snow = -1.0, p_ice = 0.0, p_snow = 0.0, max_ice = -100.0;
    for (int s = 1; s <= 8000 && (t_ice < 0.0 || t_snow < 0.0); s++) {
        thermo_step(th, w, 1.0, eye);
        if (s % 300 == 0) {
            thermo_add_fuel(th, w, -10, GROUND, 0, CAMPFIRE_FUEL_MAX);
            thermo_add_fuel(th, w, 11, GROUND, 0, CAMPFIRE_FUEL_MAX);
        }
        if (s == 600) {
            p_ice = melt_power(th, w, -9, -10, 2.2);
            p_snow = melt_power(th, w, 10, 11, 0.15);
        }
        if (t_ice < 0.0 && world_get(w, -9, GROUND, 0) == B_ICE)
            max_ice = fmax(max_ice, (double)thermo_block_temp(th, w, -9, GROUND, 0));
        if (t_ice < 0.0 && world_get(w, -9, GROUND, 0) == B_WATER) t_ice = s;
        if (t_snow < 0.0 && world_get(w, 10, GROUND, 0) == B_WATER) t_snow = s;
    }
    double est_ice = 917.0 * 334000.0 / p_ice / HEALTH_CLOCK, est_snow = 400.0 * 334000.0 / p_snow / HEALTH_CLOCK;
    CHECK(p_ice > 900.0 && p_ice < 1500.0);
    CHECK(p_snow > 500.0 && p_snow < 750.0);
    CHECK(fabs(t_ice / est_ice - 1.0) < 0.15);
    CHECK(fabs(t_snow / est_snow - 1.0) < 0.15);
    CHECK(max_ice < 1e-6); /* held at the melting point */
    /* Volume conserved: 0.917 m^3 of water from ice, 0.4 from snow. */
    CHECK(world_water_level(w, -9, GROUND, 0) == 7);
    CHECK(world_water_level(w, 10, GROUND, 0) == 3);
    CHECK(fabsf(thermo_block_temp(th, w, -9, GROUND, 0)) < 5.0f); /* meltwater starts near 0 C */
    detach(&tw);
    thermo_destroy(th);
    tw_free(&tw);
}

static void test_freezing(void)
{
    test_world tw;
    tw_init(&tw);
    thermo *th = thermo_create(7);
    world *w = &tw.w;
    const int y = 115; /* ambient -7.5 C +/- 5: always freezing */
    int ponds[2][2] = {{0, 0}, {20, 0}}; /* one by the player, one out of range */
    for (int p = 0; p < 2; p++) {
        int x = ponds[p][0], z = ponds[p][1];
        world_set(w, x, y - 1, z, B_STONE, 0);
        world_set(w, x + 1, y, z, B_STONE, 0);
        world_set(w, x - 1, y, z, B_STONE, 0);
        world_set(w, x, y, z + 1, B_STONE, 0);
        world_set(w, x, y, z - 1, B_STONE, 0);
        world_set(w, x, y, z, B_WATER, 0);
    }
    /* Water at sea level by the player never freezes. */
    world_set(w, 1, GROUND, 1, B_WATER, 0);
    dvec3 eye = dv3(2.5, y + 1, 0.5);
    thermo_set_day_time(th, 0.375); /* 09:00: the day's mean */
    double amb = (double)thermo_ambient(th, w, 0.5, y + 0.5, 0.5);
    CHECK(fabs(amb + 7.5) < 0.1);
    thermo_set_day_time(th, 0.0);

    /* A 10 cm skin, 33 MJ, released through the surface film (h = 10 W/K)
     * and five stone faces (2k = 5 W/K each, the stone barely warms: 2 MJ/K)
     * at the day's mean: 35 W/K * 7.5 K = 262 W, 1770 real s (26 survival
     * hours; Stefan's law gives ~19 h for 10 cm at -7.5 C). */
    double est = 0.1 * 1000.0 * 334000.0 / ((10.0 + 5.0 * 5.0) * -amb) / HEALTH_CLOCK;
    double t_freeze = -1.0;
    for (int s = 1; s <= 4000 && t_freeze < 0.0; s++) {
        thermo_step(th, w, 1.0, eye);
        if (world_get(w, 0, y, 0) == B_ICE) t_freeze = s;
    }
    CHECK(fabs(t_freeze / est - 1.0) < 0.2);
    CHECK(world_get(w, 20, y, 0) == B_WATER);
    thermo_step(th, w, 1.0, dv3(1.5, GROUND + 1, 1.5));
    CHECK(world_get(w, 1, GROUND, 1) == B_WATER);

    /* The new ice is a skin over water: a fire on it melts it back in a
     * ninth of the time a solid block takes (33 of 306 MJ). */
    hooks h;
    attach(&tw, &h, th);
    world_set(w, 0, y + 1, 0, B_CAMPFIRE, 0);
    double t_melt = -1.0;
    for (int s = 1; s <= 1500 && t_melt < 0.0; s++) {
        thermo_step(th, w, 1.0, eye);
        if (s % 300 == 0) thermo_add_fuel(th, w, 0, y + 1, 0, CAMPFIRE_FUEL_MAX);
        if (world_get(w, 0, y, 0) == B_WATER) t_melt = s;
    }
    CHECK(t_melt > 200.0 && t_melt < 1000.0);
    CHECK(world_water_level(w, 0, y, 0) == 7);
    detach(&tw);
    thermo_destroy(th);
    tw_free(&tw);
}

static int finite_f(float v) { return isfinite(v); }

static void test_limits(void)
{
    test_world tw;
    tw_init(&tw);
    thermo *th = thermo_create(8);
    hooks h;
    attach(&tw, &h, th);
    world *w = &tw.w;
    dvec3 eye = dv3(0.5, GROUND, 0.5);
    int placed = 0;
    for (int z = -48; z <= 48; z += 6)
        for (int x = -48; x <= 48; x += 6) placed += world_set(w, x, GROUND, z, B_CAMPFIRE, 0);
    CHECK(placed == 289);
    CHECK(thermo_fire_count(th) <= 256);
    advance(th, w, 10.0, 1.0 / 60.0, eye);
    CHECK(thermo_cell_count(th) <= THERMO_MAX_CELLS);
    int ok = 1;
    for (int x = -50; x <= 50; x += 5)
        for (int y = 0; y < WORLD_H; y += 7) {
            float a = thermo_air(th, w, x + 0.5, y + 0.5, 0.5), b = thermo_block_temp(th, w, x, y, 0);
            ok &= finite_f(a) && finite_f(b) && a > -60.0f && a < 900.0f && b > -60.0f && b < 900.0f;
            ok &= isfinite(thermo_radiant(th, w, dv3(x + 0.5, y + 0.5, 0.5)));
        }
    CHECK(ok);

    /* Garbage in: nothing breaks. */
    const double bad = (double)NAN, inf = (double)INFINITY;
    thermo_step(th, w, bad, eye);
    thermo_step(th, w, -1.0, eye);
    thermo_step(th, w, inf, eye);
    thermo_step(th, w, 1e300, dv3(bad, bad, bad));
    thermo_step(th, w, 1.0 / 60.0, dv3(1e300, -1e300, 1e300));
    thermo_set_day_time(th, bad);
    CHECK(finite_f(thermo_daylight(th)));
    CHECK(finite_f(thermo_air(th, w, bad, inf, -inf)));
    CHECK(thermo_radiant(th, w, dv3(bad, 0.0, 0.0)) == 0.0);
    CHECK(finite_f(thermo_air(th, w, 0.5, GROUND + 1.5, 0.5)));
    CHECK(thermo_cell_count(th) <= THERMO_MAX_CELLS);
    detach(&tw);
    thermo_destroy(th);
    tw_free(&tw);
}

void test_thermo_all(void)
{
    test_sun();
    test_ambient();
    test_fire_air_and_radiation();
    test_fuel();
    test_melting();
    test_freezing();
    test_limits();
}
