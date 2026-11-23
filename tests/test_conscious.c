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

void test_conscious_all(void)
{
    test_cons_greyout();
    test_cons_dim();
    test_cons_control();
    test_cons_lapses();
    test_cons_steps();
    test_cons_hurt_brain();
    test_cons_cold();
    test_cons_choke();
}
