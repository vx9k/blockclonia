/* Fear of a hunting predator: how frightening the wolves around a player
 * are, the startle rush and steady release it sets off, the inner voice,
 * and that it fades, sleeps and replays like the rest of the body. */
#include "health.h"
#include "mem.h"
#include "mob.h"
#include "test_util.h"
#include "thoughts.h"

#include <string.h>

#define NIGHT_LIGHT 0.1f
#define DAY_LIGHT 1.0f

static int logged(const health *h, const char *what)
{
    for (int i = 0; i < HEALTH_LOG; i++)
        if (strstr(h->log[i], what)) return 1;
    return 0;
}

/* Only a hunting wolf frightens, the more the nearer, and a pack more than
 * its nearest member. */
static void test_fear_mobs(void)
{
    mobs ms = {0};
    mobs_init(&ms, 7);
    player p = {0};
    p.pos = dv3(0.5, 10.0, 0.5);
    CHECK(mobs_fear(&ms, &p, NIGHT_LIGHT) == 0.0f);

    mobs_add(&ms, MOB_BOAR, dv3(2.5, 10.0, 0.5));
    CHECK(mobs_fear(&ms, &p, NIGHT_LIGHT) == 0.0f); /* boars don't hunt */

    int w = mobs_add(&ms, MOB_WOLF, dv3(30.5, 10.0, 0.5));
    CHECK(mobs_fear(&ms, &p, NIGHT_LIGHT) == 0.0f); /* beyond its hunting range */
    ms.m[w].body.pos = dv3(12.5, 10.0, 0.5);
    float far = mobs_fear(&ms, &p, NIGHT_LIGHT);
    CHECK(far > 0.3f && far < 0.5f);
    ms.m[w].body.pos = dv3(5.5, 10.0, 0.5);
    float nearer = mobs_fear(&ms, &p, NIGHT_LIGHT);
    CHECK(nearer > far && nearer < 1.0f);
    ms.m[w].body.pos = dv3(1.0, 10.0, 0.5);
    CHECK(mobs_fear(&ms, &p, NIGHT_LIGHT) == 1.0f); /* at its teeth */

    /* By day a wolf leaves people alone, unless it was hurt. */
    ms.m[w].body.pos = dv3(5.5, 10.0, 0.5);
    CHECK(mobs_fear(&ms, &p, DAY_LIGHT) == 0.0f);
    ms.m[w].angry_t = 10.0f;
    CHECK(mobs_fear(&ms, &p, DAY_LIGHT) == nearer);

    /* A second wolf as near: worse than one, never past 1. */
    mobs_add(&ms, MOB_WOLF, dv3(0.5, 10.0, 5.5));
    float pack = mobs_fear(&ms, &p, NIGHT_LIGHT);
    CHECK(pack > nearer && pack < 1.0f);
}

/* A wolf closing in startles a rush, and the hunt keeps the adrenaline up:
 * the pulse climbs over a body left in peace. */
static void test_fear_rush(void)
{
    health *h = new_body(120), *calm = new_body(120);
    health_env e = calm_env(), quiet = calm_env();
    live(h, &e, 5.0);
    live(calm, &quiet, 5.0);
    e.fear = 0.9f;
    live(h, &e, 3.0);
    live(calm, &quiet, 3.0);
    CHECK(h->surge > 0.0f && h->fear > 0.8f);
    CHECK(logged(h, "something is hunting you"));
    live(h, &e, 40.0);
    live(calm, &quiet, 40.0);
    CHECK(h->catechol > 0.6f && calm->catechol < 0.1f);
    CHECK(h->hr > calm->hr + 10.0f);
    CHECK(thoughts_level(h, FEEL_RUSH) >= 1);
    mem_free(h);
    mem_free(calm);
}

/* Hunted for three minutes on a cold night, standing or jogging: fear
 * raises the pulse and the pressure some tens of mmHg, as a defence
 * reaction does, but never into a hypertensive emergency (the playtest's
 * 239/183 jogging with a rush). */
static void test_fear_pressure(void)
{
    float sbp_still = 0.0f, dbp_still = 0.0f, sbp_jog = 0.0f, dbp_jog = 0.0f, rise = 0.0f;
    for (uint32_t seed = 4; seed <= 9; seed++) {
        for (int jog = 0; jog <= 1; jog++) {
            health *h = new_body(seed);
            health_env e = calm_env();
            e.air_temp = e.contact_temp = 8.0;
            live(h, &e, 30.0);
            const float hr0 = h->hr;
            e.fear = 1.0f;
            e.speed = jog ? 3.0 : 0.0;
            float s_max = 0.0f, d_max = 0.0f;
            for (int s = 0; s < 180; s++) {
                live(h, &e, 1.0);
                s_max = fmaxf(s_max, h->sbp);
                d_max = fmaxf(d_max, h->dbp);
            }
            if (!jog) rise += (h->hr - hr0) / 6.0f;
            *(jog ? &sbp_jog : &sbp_still) = fmaxf(jog ? sbp_jog : sbp_still, s_max);
            *(jog ? &dbp_jog : &dbp_still) = fmaxf(jog ? dbp_jog : dbp_still, d_max);
            mem_free(h);
        }
    }
    CHECK(sbp_still < 180.0f && dbp_still < 125.0f);
    CHECK(sbp_jog < 215.0f && dbp_jog < 110.0f);
    CHECK(rise > 15.0f); /* still a fright: the pulse climbs */
}

/* One startle per fright: a hunt that goes on does not keep restarting
 * the surge, it only keeps the steady release going. */
static void test_fear_once(void)
{
    health *h = new_body(121);
    health_env e = calm_env();
    e.fear = 1.0f;
    int starts = 0;
    float was = h->surge;
    for (int s = 0; s < 120 * 60; s++) {
        health_step(h, &e, PHYS_DT);
        if (h->surge > was + 1.0f) starts++;
        was = h->surge;
    }
    CHECK(starts == 1);
    /* The steady release alone holds a strong level; the startle surge is
     * long over. */
    CHECK(h->surge == 0.0f && h->catechol > 0.9f);
    mem_free(h);
}

/* When the wolf is gone the fear goes with it, and the adrenaline clears
 * over minutes. */
static void test_fear_fades(void)
{
    health *h = new_body(122);
    health_env e = calm_env();
    e.fear = 0.8f;
    live(h, &e, 30.0);
    float peak = h->catechol;
    e.fear = 0.0f;
    live(h, &e, 10.0);
    CHECK(h->fear < 0.01f && thoughts_level(h, FEEL_FEAR) == 0);
    live(h, &e, 600.0);
    CHECK(h->catechol < 0.25f * peak);
    mem_free(h);
}

/* An unconscious body feels no fear and makes no adrenaline for it. */
static void test_fear_unconscious(void)
{
    health *h = new_body(123);
    health_env e = calm_env();
    h->concussion = 120.0f;
    live(h, &e, 1.0);
    CHECK(!health_awake(h));
    e.fear = 1.0f;
    live(h, &e, 30.0);
    CHECK(h->fear == 0.0f && h->catechol < 0.1f);
    CHECK(!logged(h, "something is hunting you"));
    mem_free(h);
}

static void test_fear_feel(void)
{
    health *h = new_body(124);
    CHECK(thoughts_level(h, FEEL_FEAR) == 0);
    h->fear = 0.2f;
    CHECK(thoughts_level(h, FEEL_FEAR) == 1);
    h->fear = 0.5f;
    CHECK(thoughts_level(h, FEEL_FEAR) == 2);
    h->fear = 0.9f;
    CHECK(thoughts_level(h, FEEL_FEAR) == 3);
    for (int l = 0; l <= 3; l++) CHECK(thoughts_lines(FEEL_FEAR, l) > 0);
    mem_free(h);
}

/* The new state rides the wire and replays exactly. */
static void test_fear_wire_replay(void)
{
    health *a = new_body(125), *b = new_body(125), *c = new_body(1);
    health_env e = calm_env();
    e.fear = 0.7f;
    live(a, &e, 20.0);
    live(b, &e, 20.0);
    CHECK(a->fear == b->fear && a->fear_ref == b->fear_ref && a->catechol == b->catechol && a->hr == b->hr);
    uint8_t *buf = mem_alloc(HEALTH_NET_MAX);
    size_t n = health_encode(a, buf, HEALTH_NET_MAX);
    CHECK(n > 0 && health_decode(c, buf, n) == 0);
    CHECK(c->fear == a->fear && c->fear_ref == a->fear_ref);
    mem_free(buf);
    mem_free(a);
    mem_free(b);
    mem_free(c);
}

void test_fear_all(void)
{
    TEST_PART(test_fear_mobs);
    TEST_PART(test_fear_rush);
    TEST_PART(test_fear_once);
    TEST_PART(test_fear_pressure);
    TEST_PART(test_fear_fades);
    TEST_PART(test_fear_unconscious);
    TEST_PART(test_fear_feel);
    TEST_PART(test_fear_wire_replay);
}
