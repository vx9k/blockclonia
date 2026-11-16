/* The vasovagal reflex: what sets it off, the faint it brings on standing,
 * crouching to head it off, and the quick recovery once the body is down. */
#include "health.h"
#include "mem.h"
#include "physics.h"
#include "survival.h"
#include "test_util.h"
#include "thoughts.h"

#include <string.h>

static int logged(const health *h, const char *what)
{
    for (int i = 0; i < HEALTH_LOG; i++)
        if (strstr(h->log[i], what)) return 1;
    return 0;
}

/* Standing: the pulse and the pressure fall together, the brain runs
 * short and the body goes down within ~20 s, then comes round. */
static void test_faint_standing(void)
{
    health *h = new_body(90);
    health_env e = calm_env();
    health_injure(h, health_debug_find("faint"), -1);
    float low_hr = 1e3f, low_map = 1e3f;
    int out_at = -1;
    for (int s = 0; s < 25 && out_at < 0; s++) {
        live(h, &e, 1.0);
        low_hr = h->hr < low_hr ? h->hr : low_hr;
        low_map = h->map < low_map ? h->map : low_map;
        if (h->conscious == CONS_UNCONSCIOUS) out_at = s;
    }
    CHECK(out_at >= 8 && out_at <= 22);
    CHECK(low_hr < 45.0f && low_map < 40.0f);
    CHECK(logged(h, "Lost consciousness"));
    live(h, &e, 15.0);
    CHECK(h->conscious != CONS_UNCONSCIOUS && h->map > 75.0f && h->vagal < 0.3f);
    live(h, &e, 30.0);
    CHECK(h->vagal < 0.05f && h->faint_drive == 0.0f && !h->dead);
    mem_free(h);
}

/* Crouching keeps the head near the heart and blunts the reflex. */
static void test_faint_crouched(void)
{
    health *h = new_body(90);
    health_env e = calm_env();
    e.crouched = 1;
    health_injure(h, health_debug_find("faint"), -1);
    float low_map = 1e3f;
    for (int s = 0; s < 40; s++) {
        live(h, &e, 1.0);
        CHECK(h->conscious != CONS_UNCONSCIOUS);
        low_map = h->map < low_map ? h->map : low_map;
    }
    CHECK(low_map > 60.0f && low_map < 85.0f);
    mem_free(h);
}

/* What the player notices: greying vision, the voice, the HUD hint. */
static void test_faint_feel(void)
{
    health *h = new_body(91);
    CHECK(thoughts_level(h, FEEL_SWOON) == 0);
    CHECK(health_get_senses(h).tunnel < 0.1f);
    h->vagal = 0.9f;
    CHECK(thoughts_level(h, FEEL_SWOON) == 3);
    health_senses v = health_get_senses(h);
    CHECK(v.tunnel > 0.9f && v.sway > 0.3f);
    h->vagal = 0.2f;
    CHECK(thoughts_level(h, FEEL_SWOON) == 1);
    for (int l = 0; l <= 3; l++) CHECK(thoughts_lines(FEEL_SWOON, l) > 0);
    mem_free(h);
}

/* A sudden severe pain sets it off in some bodies, and then cuts the
 * adrenaline rush short. */
static void test_faint_from_pain(void)
{
    int faints = 0, short_rush = 1;
    for (int i = 0; i < 60; i++) {
        health *h = new_body(400 + (uint32_t)i);
        health_env e = calm_env();
        live(h, &e, 1.0);
        h->part[BP_LLEG].pain_spike = 10.0f;
        live(h, &e, 2.0);
        if (h->faint_drive > 0.0f) {
            faints++;
            short_rush &= h->surge <= 5.0f && logged(h, "go pale");
        }
        mem_free(h);
    }
    CHECK(faints >= 3 && faints <= 25);
    CHECK(short_rush);

    /* A small pain never does. */
    health *h = new_body(470);
    health_env e = calm_env();
    live(h, &e, 1.0);
    h->part[BP_LARM].pain_spike = 2.0f;
    live(h, &e, 2.0);
    CHECK(h->faint_drive == 0.0f && h->vagal < 0.01f);
    mem_free(h);
}

/* A fresh rush of blood can set it off; blood the eyes have got used to
 * cannot. */
static void test_faint_from_blood(void)
{
    int faints = 0, used = 0;
    for (int i = 0; i < 60; i++) {
        health *h = new_body(500 + (uint32_t)i);
        health_env e = calm_env();
        live(h, &e, 1.0);
        health_cut(h, BP_LARM, 0.7f, 1, 0.1f);
        live(h, &e, 1.0);
        faints += h->faint_drive > 0.0f && logged(h, "sight of the blood");
        CHECK(h->blood_seen > 30.0f);
        mem_free(h);

        h = new_body(500 + (uint32_t)i);
        live(h, &e, 1.0);
        h->blood_seen = 1e4f;
        health_cut(h, BP_LARM, 0.7f, 1, 0.1f);
        live(h, &e, 1.0);
        used += h->faint_drive > 0.0f;
        mem_free(h);
    }
    CHECK(faints >= 2 && faints <= 20);
    CHECK(used == 0);
}

/* Once running it doesn't restart, and an unconscious body has none. */
static void test_faint_once(void)
{
    health *h = new_body(92);
    health_env e = calm_env();
    health_injure(h, health_debug_find("faint"), -1);
    live(h, &e, 5.0);
    float left = h->faint_drive;
    health_injure(h, health_debug_find("faint"), -1);
    CHECK(h->faint_drive > left); /* the debug entry forces it */
    h->faint_drive = 0.0f;
    h->vagal = 0.0f;
    h->conscious = CONS_UNCONSCIOUS;
    h->blood_seen = 0.0f;
    health_cut(h, BP_LARM, 0.9f, 1, 0.1f);
    live(h, &e, 0.5);
    CHECK(h->vagal < 0.05f);
    mem_free(h);
}

/* Same seed, same faint, to the bit. */
static void test_faint_replay(void)
{
    health *a = new_body(93), *b = new_body(93);
    health_env e = calm_env();
    health_cut(a, BP_RLEG, 0.8f, 1, 0.1f);
    health_cut(b, BP_RLEG, 0.8f, 1, 0.1f);
    a->faint_drive = b->faint_drive = 20.0f;
    live(a, &e, 30.0);
    live(b, &e, 30.0);
    CHECK(a->vagal == b->vagal && a->map == b->map && a->hr == b->hr && a->conscious == b->conscious);
    mem_free(a);
    mem_free(b);
}

/* Sneaking on the ground is crouching; in the air it is not. */
static void test_faint_env(void)
{
    test_world t = {0};
    tw_init(&t);
    player p = {0};
    player_spawn(&p, &t.w, 0.5, 0.5);
    p.on_ground = 1;
    p.sneaking = 1;
    survival_before b = survival_capture(&p);
    health_env e = {0};
    survival_env(&t.w, &p, &b, 0, &e);
    CHECK(e.crouched == 1);
    p.on_ground = 0;
    survival_env(&t.w, &p, &b, 0, &e);
    CHECK(e.crouched == 0);
    p.on_ground = 1;
    p.sneaking = 0;
    survival_env(&t.w, &p, &b, 0, &e);
    CHECK(e.crouched == 0);
    tw_free(&t);
}

void test_faint_all(void)
{
    test_faint_standing();
    test_faint_crouched();
    test_faint_feel();
    test_faint_from_pain();
    test_faint_from_blood();
    test_faint_once();
    test_faint_replay();
    test_faint_env();
}
