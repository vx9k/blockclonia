/* Consciousness as a scale (#122 stage 3): a level from alert down to
 * unresponsive, set by the worst of what the brain is suffering, that a
 * body passes through step by step instead of switching off. */
#include "health.h"
#include "mem.h"
#include "physics.h"
#include "test_util.h"

#include <math.h>

/* The steps lie in order along the level, and only the bottom three
 * close the eyes. */
static void test_cons_steps(void)
{
    CHECK(health_cons_of(1.0f) == CONS_ALERT);
    CHECK(health_cons_of(0.7f) == CONS_CONFUSED);
    CHECK(health_cons_of(0.4f) == CONS_DROWSY);
    CHECK(health_cons_of(0.25f) == CONS_VOICE);
    CHECK(health_cons_of(0.15f) == CONS_PAIN);
    CHECK(health_cons_of(0.0f) == CONS_UNCONSCIOUS);
    int ordered = 1;
    for (int i = 0; i < 100; i++) ordered &= health_cons_of(0.01f * (float)i) <= health_cons_of(0.01f * (float)(i + 1));
    CHECK(ordered);
    health *h = new_body(1);
    CHECK(h->level == 1.0f && h->conscious == CONS_ALERT && health_awake(h));
    h->conscious = CONS_DROWSY;
    CHECK(health_awake(h));
    h->conscious = CONS_VOICE;
    CHECK(!health_awake(h));
    mem_free(h);
}

/* A brain at 90 % is not cosmetic: the level is already a little down,
 * though the body still counts as alert. */
static void test_cons_hurt_brain(void)
{
    health *h = new_body(1);
    health_env e = calm_env();
    h->organ[ORG_BRAIN] = 0.9f;
    live(h, &e, 20.0);
    CHECK(h->level < 0.99f && h->level > 0.85f);
    CHECK(h->conscious == CONS_ALERT);
    mem_free(h);
}

/* Mild hypothermia (34 C) clouds the mind without putting it out. */
static void test_cons_cold(void)
{
    health *h = new_body(1);
    health_env e = calm_env();
    for (int i = 0; i < 20 * 60; i++) {
        h->temp = 34.0f;
        health_step(h, &e, PHYS_DT);
    }
    CHECK(health_awake(h) && h->conscious < CONS_ALERT);
    mem_free(h);
}

/* Choking: the body goes down through every waking step before its eyes
 * close, never jumping back up while the airway stays shut, and comes
 * round again once air returns. */
static void test_cons_choke(void)
{
    health *h = new_body(2);
    health_env e = calm_env();
    live(h, &e, 30.0);
    e.airway = AIRWAY_BLOCKED;
    int seen[CONS_ALERT + 1] = {0}, rose = 0, lowest = CONS_ALERT;
    for (int i = 0; i < 240 * 60 && health_awake(h); i++) {
        health_step(h, &e, PHYS_DT);
        seen[h->conscious] = 1;
        rose |= h->conscious > lowest;
        if (h->conscious < lowest) lowest = h->conscious;
    }
    CHECK(seen[CONS_CONFUSED] && seen[CONS_DROWSY]);
    CHECK(!health_awake(h) && !rose);
    e.airway = AIRWAY_AIR;
    int back = 0;
    for (int s = 0; s < 600 && !back && !h->dead; s++) {
        live(h, &e, 1.0);
        back = h->conscious == CONS_ALERT;
    }
    CHECK(back);
    mem_free(h);
}

/* What each step does to the hands: nothing when alert, then slower,
 * clumsier and more unsteady the further down the scale. */
static void test_cons_control(void)
{
    health *h = new_body(1);
    health_senses s = health_get_senses(h);
    CHECK(s.lag == 0.0f && s.drop == 0.0f && s.stumble == 0.0f && s.blackout == 0.0f);
    const float sway0 = s.sway;
    h->level = 0.7f;
    h->conscious = health_cons_of(h->level);
    s = health_get_senses(h);
    CHECK(s.lag > 0.05f && s.lag < 0.2f && s.drop > 0.0f && s.stumble > 0.0f && s.sway > sway0);
    const float lag_conf = s.lag, drop_conf = s.drop, stumble_conf = s.stumble;
    h->level = 0.4f;
    h->conscious = health_cons_of(h->level);
    s = health_get_senses(h);
    CHECK(s.lag > 0.25f && s.lag > lag_conf && s.drop > drop_conf && s.stumble > stumble_conf);
    mem_free(h);
}

/* Holds a body at 31.5 C for ten minutes, deep enough in the cold to keep
 * it drowsy, and counts its blackouts. */
static int count_lapses(health *h, float temp, int *longest_ok, int *control_lost, int *lay_down)
{
    health_env e = calm_env();
    int lapses = 0;
    float was = 0.0f, len = 0.0f;
    *longest_ok = 1;
    *control_lost = 1;
    *lay_down = 0;
    for (int i = 0; i < 600 * 60; i++) {
        h->temp = temp;
        health_step(h, &e, PHYS_DT);
        if (h->lapse > 0.0f && was <= 0.0f) {
            lapses++;
            len = 0.0f;
        }
        if (h->lapse > 0.0f) {
            len += (float)PHYS_DT;
            *control_lost &= !health_get_limits(h).has_control;
            *lay_down |= h->down > 0.0f;
        } else if (was > 0.0f) {
            *longest_ok &= len >= 0.9f && len <= 3.1f;
        }
        was = h->lapse;
    }
    return lapses;
}

/* A drowsy body drifts off for a second or three now and then, losing
 * the controls but not falling into the long lie of a faint; an alert one
 * never does. */
static void test_cons_lapses(void)
{
    health *h = new_body(3);
    int len_ok = 0, lost = 0, lay = 0;
    int n = count_lapses(h, 31.5f, &len_ok, &lost, &lay);
    CHECK(health_awake(h) && h->conscious == CONS_DROWSY);
    CHECK(n >= 3 && n <= 60 && len_ok && lost && !lay);
    health *a = new_body(3);
    CHECK(count_lapses(a, 37.0f, &len_ok, &lost, &lay) == 0);
    mem_free(h);
    mem_free(a);
}

/* A brain short of oxygen loses colour first, then the edges of the view,
 * the order pilots report under g; a clear one sees normally. */
static void test_cons_greyout(void)
{
    health *h = new_body(1);
    health_senses s = health_get_senses(h);
    CHECK(s.grey == 0.0f && s.tunnel == 0.0f && s.dim == 0.0f);
    h->brain_o2 = 0.75f;
    s = health_get_senses(h);
    CHECK(s.grey > 0.3f && s.tunnel < 0.05f);
    h->brain_o2 = 0.6f;
    s = health_get_senses(h);
    CHECK(s.grey > 0.9f && s.tunnel > 0.3f && s.tunnel < 0.9f);
    h->brain_o2 = 0.45f;
    s = health_get_senses(h);
    CHECK(s.tunnel > 0.95f);
    mem_free(h);
}

/* Drowsy eyes let in less light, and a blackout is black. */
static void test_cons_dim(void)
{
    health *h = new_body(2);
    h->level = 0.4f;
    h->conscious = health_cons_of(h->level);
    health_senses s = health_get_senses(h);
    CHECK(s.dim > 0.2f && s.dim < 0.6f);
    h->lapse = 2.0f;
    s = health_get_senses(h);
    CHECK(s.dim == 1.0f && s.blackout == 1.0f);
    mem_free(h);
}

/* Holds a body's brain at a level of damage (or, with knocked, out cold
 * from a blow) for ten minutes after a minute to settle, and
 * counts the moments its eyes open on the world: their number, whether
 * each lasted 0.3-1.3 s, and whether what showed was dim, grey and blurred
 * rather than clear. */
static void hold(health *h, float brain, int knocked)
{
    h->organ[ORG_BRAIN] = brain;
    if (knocked) h->concussion = 30.0f;
}

static int count_glimpses(uint32_t seed, float brain, int knocked, int *cons, int *len_ok, int *seen_fogged,
                          int *dark_between)
{
    health *h = new_body(seed);
    health_env e = calm_env();
    *len_ok = *seen_fogged = *dark_between = 1;
    for (int i = 0; i < 60 * 60; i++) {
        hold(h, brain, knocked);
        health_step(h, &e, PHYS_DT);
    }
    *cons = h->conscious;
    int n = 0;
    float was = 0.0f, len = 0.0f;
    for (int i = 0; i < 600 * 60 && !h->dead; i++) {
        hold(h, brain, knocked);
        health_step(h, &e, PHYS_DT);
        const health_senses s = health_get_senses(h);
        if (h->glimpse > 0.0f && was <= 0.0f) {
            n++;
            len = 0.0f;
        }
        if (h->glimpse > 0.0f) {
            len += (float)PHYS_DT;
            *seen_fogged &= s.dim < 1.0f && s.dim >= 0.3f && s.grey >= 0.6f && s.blur >= 0.7f && s.tunnel >= 0.4f;
        } else {
            if (was > 0.0f) *len_ok &= len >= 0.3f && len <= 1.3f;
            *dark_between &= s.dim == 1.0f;
        }
        was = h->glimpse;
    }
    mem_free(h);
    return n;
}

/* Out of reach of the world, the eyes still open on it now and then: more
 * often for a body that answers to a voice than one that answers only to
 * pain, never for one past reach; between, it is black. */
static void test_cons_glimpses(void)
{
    int cons = 0, len_ok = 0, fogged = 0, dark = 0;
    const int voice = count_glimpses(5, 0.25f, 0, &cons, &len_ok, &fogged, &dark);
    CHECK(cons == CONS_VOICE && voice >= 30 && voice <= 130 && len_ok && fogged && dark);
    const int pain = count_glimpses(5, 0.15f, 0, &cons, &len_ok, &fogged, &dark);
    CHECK(cons == CONS_PAIN && pain >= 6 && pain < voice && len_ok && fogged && dark);
    const int none = count_glimpses(5, 1.0f, 1, &cons, &len_ok, &fogged, &dark);
    CHECK(cons == CONS_UNCONSCIOUS && none == 0 && dark);
}

void test_conscious_all(void)
{
    test_cons_glimpses();
    test_cons_greyout();
    test_cons_dim();
    test_cons_control();
    test_cons_lapses();
    test_cons_steps();
    test_cons_hurt_brain();
    test_cons_cold();
    test_cons_choke();
}
