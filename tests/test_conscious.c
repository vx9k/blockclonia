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
    for (float l = 0.0f; l < 1.0f; l += 0.01f) ordered &= health_cons_of(l) <= health_cons_of(l + 0.01f);
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

void test_conscious_all(void)
{
    test_cons_steps();
    test_cons_hurt_brain();
    test_cons_cold();
    test_cons_choke();
}
