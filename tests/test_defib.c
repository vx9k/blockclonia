/* The defibrillator (health.c): what a shock does to fibrillating, stopped
 * and beating hearts, the automatic device's analysis, charge and
 * countdown, and the item that carries it (item.c, interact.c,
 * inventory.c). */
#include "hud.h"
#include "interact.h"
#include "mem.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

/* Fraction of n bodies (seeds base..) in VF for `arrest_s` whose single
 * shock brings a heartbeat back. */
static float rosc_rate(uint32_t base, int n, float arrest_s, float temp)
{
    int back = 0;
    for (int i = 0; i < n; i++) {
        health *h = new_body(base + (uint32_t)i);
        health_arrest(h, RHYTHM_VF);
        h->arrest_time = arrest_s;
        h->temp = temp;
        health_shock(h);
        back += h->rhythm == RHYTHM_SINUS;
        mem_free(h);
    }
    return (float)back / (float)n;
}

/* Only VF responds, less the longer it has lasted, and a cold heart less. */
static void test_shock_outcomes(void)
{
    float early = rosc_rate(1000, 400, 20.0f, 37.0f), late = rosc_rate(2000, 400, 240.0f, 37.0f);
    CHECK(early > 0.78f && early < 0.95f);
    CHECK(late > 0.35f && late < 0.65f && early > late + 0.2f);
    CHECK(rosc_rate(3000, 200, 20.0f, 27.0f) < 0.5f);
    /* Asystole is never shocked back. */
    int back = 0;
    for (int i = 0; i < 100; i++) {
        health *h = new_body(4000 + (uint32_t)i);
        health_arrest(h, RHYTHM_ASYSTOLE);
        health_shock(h);
        back += h->rhythm != RHYTHM_ASYSTOLE;
        mem_free(h);
    }
    CHECK(back == 0);
    /* The same seed gives the same outcome. */
    health *a = new_body(77), *b = new_body(77);
    health_arrest(a, RHYTHM_VF);
    health_arrest(b, RHYTHM_VF);
    health_shock(a);
    health_shock(b);
    CHECK(a->rhythm == b->rhythm && a->rng == b->rng && a->part[BP_CHEST].burn_omega == b->part[BP_CHEST].burn_omega);
    mem_free(a);
    mem_free(b);
}

/* A beating heart gains nothing: the chest hurts, the skin under the pads
 * heats, and only a shock on the T wave's upstroke starts VF. */
static void test_shock_beating_heart(void)
{
    health_env e = calm_env();
    int vf_off = 0, vf_on = 0, on_tries = 0;
    for (int i = 0; i < 60; i++) {
        health *h = new_body(500 + (uint32_t)i);
        live(h, &e, 10.0);
        /* Wait for a moment well away from the T wave: just after an R. */
        while (h->t - h->last_r > 0.02) health_step(h, &e, 1.0 / 60.0);
        float omega = h->part[BP_CHEST].burn_omega;
        health_shock(h);
        vf_off += h->rhythm != RHYTHM_SINUS;
        CHECK(h->part[BP_CHEST].burn_omega > omega && h->part[BP_CHEST].pain_spike > 9.0f);
        mem_free(h);

        /* And on the T wave's upstroke. */
        h = new_body(900 + (uint32_t)i);
        live(h, &e, 10.0);
        for (int k = 0; k < 600; k++) {
            float qt = sqrtf(60.0f / h->hr), since = (float)(h->t - h->last_r);
            if (since > 0.2f * qt && since < 0.25f * qt) break;
            health_step(h, &e, 1.0 / 240.0);
        }
        float qt = sqrtf(60.0f / h->hr), since = (float)(h->t - h->last_r);
        if (since > 0.2f * qt && since < 0.25f * qt) {
            on_tries++;
            health_shock(h);
            vf_on += h->rhythm == RHYTHM_VF;
        }
        mem_free(h);
    }
    CHECK(vf_off == 0);
    CHECK(on_tries > 30 && vf_on > on_tries / 5 && vf_on < on_tries);
}

/* With the pads on a healthy body the device finds nothing to shock and
 * changes nothing: the body runs exactly as its twin without pads. */
static void test_device_normal_rhythm(void)
{
    health *a = new_body(31), *b = new_body(31);
    health_env e = calm_env();
    health_pads(a, 1);
    CHECK(a->defib.phase == DEFIB_ANALYSE);
    live(a, &e, 120.0);
    live(b, &e, 120.0);
    CHECK(a->defib.phase == DEFIB_MONITOR && a->defib.advice == 0 && a->defib.shocks == 0);
    CHECK(a->rng == b->rng && a->hr == b->hr && a->map == b->map && a->rhythm == RHYTHM_SINUS);
    CHECK(a->part[BP_CHEST].burn_omega == 0.0f);
    /* Pads let go under water. */
    health_env wet = calm_env();
    wet.submerged = 1.0;
    live(a, &wet, 1.0);
    CHECK(a->defib.phase == DEFIB_OFF);
    mem_free(a);
    mem_free(b);
}

/* VF under the pads: analysis, a charge, a countdown, then the shock; the
 * arrest is not called while the device can still shock. Without pads it
 * is called after a minute, and asystole is never shocked. */
static void test_device_vf(void)
{
    health_env e = calm_env();
    health *h = new_body(41);
    health_pads(h, 1);
    live(h, &e, 7.0);
    CHECK(h->defib.phase == DEFIB_MONITOR);
    health_arrest(h, RHYTHM_VF);
    live(h, &e, 1.0);
    CHECK(h->defib.phase == DEFIB_ANALYSE);
    live(h, &e, 5.5);
    CHECK(h->defib.phase == DEFIB_CHARGE && h->defib.advice == 1);
    live(h, &e, 3.0);
    CHECK(h->defib.joules > 50.0f && h->defib.joules < 110.0f && h->defib.shocks == 0);
    live(h, &e, 4.0);
    CHECK(h->defib.phase == DEFIB_CLEAR && h->defib.shocks == 0);
    live(h, &e, 2.5);
    CHECK(h->defib.shocks == 1);
    live(h, &e, 60.0);
    CHECK(!h->dead);

    /* Many bodies: nearly all get a heartbeat back within a minute. */
    int back = 0;
    for (int i = 0; i < 40; i++) {
        health *v = new_body(6000 + (uint32_t)i);
        health_pads(v, 1);
        live(v, &e, 7.0);
        health_arrest(v, RHYTHM_VF);
        live(v, &e, 70.0);
        back += !v->dead && v->rhythm == RHYTHM_SINUS;
        mem_free(v);
    }
    CHECK(back >= 34);

    /* Replay is exact. */
    health *r1 = new_body(43), *r2 = new_body(43);
    health *rs[2] = {r1, r2};
    for (int k = 0; k < 2; k++) {
        health_pads(rs[k], 1);
        live(rs[k], &e, 7.0);
        health_arrest(rs[k], RHYTHM_VF);
        live(rs[k], &e, 40.0);
    }
    CHECK(r1->rhythm == r2->rhythm && r1->defib.shocks == r2->defib.shocks && r1->rng == r2->rng && r1->hr == r2->hr &&
          r1->organ[ORG_BRAIN] == r2->organ[ORG_BRAIN]);

    /* No pads: the arrest is called after a minute. */
    health *n = new_body(44);
    health_arrest(n, RHYTHM_VF);
    live(n, &e, 61.0);
    CHECK(n->dead && n->cause == DEATH_CARDIAC);
    /* Asystole under the pads: no shock advised, and called all the same. */
    health *s = new_body(45);
    health_pads(s, 1);
    health_arrest(s, RHYTHM_ASYSTOLE);
    live(s, &e, 30.0);
    CHECK(s->defib.advice == 0 && s->defib.shocks == 0);
    live(s, &e, 31.0);
    CHECK(s->dead);

    /* The HUD says what the device is doing. */
    enum { MAXQ = 8192 };
    ui_vertex *mem = mem_alloc(sizeof(ui_vertex) * 4 * MAXQ);
    health *c = new_body(46);
    health_pads(c, 1);
    health_arrest(c, RHYTHM_VF);
    live(c, &e, 9.0);
    hud_state hs;
    memset(&hs, 0, sizeof hs);
    hud_update(&hs, c, 0.1f);
    int found = 0;
    for (int i = 0; i < hs.alert_count; i++) found |= strstr(hs.alerts[i].text, "charging") != NULL;
    CHECK(found);
    ui u;
    ui_begin(&u, mem, MAXQ, 1280, 720);
    hs.panel = 1;
    hud_draw(&u, c, &hs);
    CHECK(u.overflow == 0);
    mem_free(mem);
    mem_free(c);
    mem_free(s);
    mem_free(n);
    mem_free(r1);
    mem_free(r2);
    mem_free(h);
}

/* Puts a body into rhythm r the way the game can: an arrest for the
 * pulseless ones, the debug entries for the ones that keep a pulse. */
static void enter_rhythm(health *h, int r)
{
    if (!health_rhythm_perfusing(r)) health_arrest(h, r);
    else if (r == RHYTHM_AF) health_injure(h, health_debug_find("afib"), -1);
    else if (r == RHYTHM_VT) health_injure(h, health_debug_find("vtach"), -1);
    else if (r == RHYTHM_AVB3) health_injure(h, health_debug_find("heart-block"), -1);
}

/* What counts as shockable: VF and pulseless VT, nothing else; and which
 * rhythms pump. Every rhythm has a monitor label that fits under the
 * bedside monitor's heart rate (84 px). */
static void test_rhythm_table(void)
{
    for (int r = 0; r < RHYTHM_COUNT; r++) {
        CHECK(health_rhythm_shockable(r) == (r == RHYTHM_VF || r == RHYTHM_PVT));
        CHECK(health_rhythm_perfusing(r) ==
              (r == RHYTHM_SINUS || r == RHYTHM_AF || r == RHYTHM_VT || r == RHYTHM_AVB3));
        CHECK(strcmp(health_rhythm_name(r), "?") != 0 && ui_text_width(health_rhythm_short(r), 1) <= 83.0f);
    }
    CHECK(!health_rhythm_shockable(RHYTHM_COUNT) && !health_rhythm_perfusing(-1));
    CHECK(!strcmp(health_rhythm_short(RHYTHM_COUNT), "?"));
    /* health_arrest takes only the pulseless rhythms. */
    health *h = new_body(60);
    health_arrest(h, RHYTHM_AF);
    health_arrest(h, RHYTHM_COUNT);
    CHECK(h->rhythm == RHYTHM_SINUS);
    health_arrest(h, RHYTHM_PEA);
    health_env e = calm_env();
    live(h, &e, 1.0);
    CHECK(h->rhythm == RHYTHM_PEA && h->hr == 0.0f && health_spo2_reading(h) == -1);
    mem_free(h);
}

/* The AED's analysis advises a shock for VF and pulseless VT only, and
 * over a minute with a non-shockable rhythm it never delivers one. */
static void test_device_classifies(void)
{
    health_env e = calm_env();
    for (int r = 1; r < RHYTHM_COUNT; r++) {
        health *h = new_body(700 + (uint32_t)r);
        health_pads(h, 1);
        live(h, &e, 7.0);
        CHECK(h->defib.phase == DEFIB_MONITOR && h->defib.seen == RHYTHM_SINUS && h->defib.advice == 0);
        enter_rhythm(h, r);
        CHECK(h->rhythm == r);
        live(h, &e, 6.5);
        /* VT can stop, and pulseless VT turn into VF, while it analyses:
         * the advice follows what it saw. */
        CHECK(h->defib.seen == r || r == RHYTHM_VT || r == RHYTHM_PVT);
        CHECK(h->defib.advice == health_rhythm_shockable(h->defib.seen));
        if (h->defib.seen == r) CHECK(h->defib.advice == health_rhythm_shockable(r));
        CHECK((h->defib.phase == DEFIB_CHARGE) == health_rhythm_shockable(h->defib.seen));
        if (r == RHYTHM_AF || r == RHYTHM_AVB3 || r == RHYTHM_PEA || r == RHYTHM_ASYSTOLE) {
            live(h, &e, 60.0);
            CHECK(h->defib.shocks == 0 && h->part[BP_CHEST].burn_omega == 0.0f);
        }
        mem_free(h);
    }
    /* Pulseless VT under the pads is shocked, and like VF usually comes back. */
    int back = 0;
    for (int i = 0; i < 40; i++) {
        health *v = new_body(7100 + (uint32_t)i);
        health_pads(v, 1);
        live(v, &e, 7.0);
        health_arrest(v, RHYTHM_PVT);
        live(v, &e, 70.0);
        CHECK(v->defib.shocks >= 1);
        back += !v->dead && v->rhythm == RHYTHM_SINUS;
        mem_free(v);
    }
    CHECK(back >= 30);
}

/* A shock through a rhythm with a pulse: AF converts about half the time,
 * VT most of the time, heart block never; any of them can land on a T
 * wave. Asystole and PEA stay. */
static void test_shock_other_rhythms(void)
{
    health_env e = calm_env();
    int af_sinus = 0, vt_sinus = 0, vt_vf = 0, block_kept = 0, pea_kept = 0;
    const int n = 200;
    for (int i = 0; i < n; i++) {
        health *h = new_body(8000 + (uint32_t)i);
        live(h, &e, 2.0);
        enter_rhythm(h, RHYTHM_AF);
        live(h, &e, 1.0);
        health_shock(h);
        af_sinus += h->rhythm == RHYTHM_SINUS;
        mem_free(h);

        h = new_body(9000 + (uint32_t)i);
        live(h, &e, 2.0);
        enter_rhythm(h, RHYTHM_VT);
        live(h, &e, 0.5);
        if (h->rhythm == RHYTHM_VT) {
            health_shock(h);
            vt_sinus += h->rhythm == RHYTHM_SINUS;
            vt_vf += h->rhythm == RHYTHM_VF;
        }
        mem_free(h);

        h = new_body(10000 + (uint32_t)i);
        live(h, &e, 2.0);
        enter_rhythm(h, RHYTHM_AVB3);
        live(h, &e, 0.5);
        health_shock(h);
        block_kept += h->rhythm == RHYTHM_AVB3 || h->rhythm == RHYTHM_VF;
        mem_free(h);

        h = new_body(11000 + (uint32_t)i);
        health_arrest(h, RHYTHM_PEA);
        health_shock(h);
        pea_kept += h->rhythm == RHYTHM_PEA;
        mem_free(h);
    }
    CHECK(af_sinus > n * 35 / 100 && af_sinus < n * 65 / 100);
    CHECK(vt_sinus > n * 55 / 100 && vt_vf > n * 8 / 100 && vt_vf < n * 25 / 100);
    CHECK(block_kept == n && pea_kept == n);
}

/* The item: in the starting kit, one per slot, carried by the INV1 save,
 * needed for the treatment but not used up, and put on by right-click. */
static void test_defib_item(void)
{
    CHECK(item_valid(I_DEFIBRILLATOR) && item_get(I_DEFIBRILLATOR)->stack == 1);
    CHECK(!strcmp(item_get(I_DEFIBRILLATOR)->name, "Defibrillator"));
    inventory inv;
    inv_starting_kit(&inv);
    CHECK(inv_count(&inv, I_DEFIBRILLATOR) == 1);
    uint8_t buf[INV_ENCODED_SIZE];
    CHECK(inv_encode(&inv, buf, sizeof buf) == INV_ENCODED_SIZE);
    inventory back;
    CHECK(inv_decode(&back, buf, sizeof buf) == 0 && inv_count(&back, I_DEFIBRILLATOR) == 1);
    for (int i = 0; i < INV_SLOTS; i++)
        if (buf[8 + 2 * i] == I_DEFIBRILLATOR) buf[8 + 2 * i + 1] = 2; /* two in a slot of one */
    CHECK(inv_decode(&back, buf, sizeof buf) == -1);

    health *h = new_body(51);
    char msg[128];
    inventory none;
    inv_init(&none);
    CHECK(health_treat(h, &none, BP_CHEST, TREAT_DEFIB, 0, msg, sizeof msg) == 0 && h->defib.phase == DEFIB_OFF);
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_DEFIB, 0, msg, sizeof msg) == 1 && h->defib.phase != DEFIB_OFF);
    CHECK(inv_count(&inv, I_DEFIBRILLATOR) == 1);
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_DEFIB, 0, msg, sizeof msg) == 1 && h->defib.phase == DEFIB_OFF);
    mem_free(h);

    /* Right-click with it in hand asks for the pads and places nothing. */
    test_world t;
    tw_init(&t);
    fx_state *fx = mem_alloc(sizeof *fx);
    fx_init(fx, 5);
    player p;
    player_spawn(&p, &t.w, 0.5, 0.5);
    p.pitch = -1.2f;
    t.ph.pl = &p;
    inventory held;
    inv_init(&held);
    inv_add(&held, I_DEFIBRILLATOR, 1);
    interact s;
    interact_init(&s, 3);
    dvec3 eye = dv3(p.pos.x, p.pos.y + PLAYER_EYE, p.pos.z);
    vec3 dir = look_dir(p.yaw, p.pitch);
    ray_hit hit = physics_raycast(&t.w, eye, dir, 5.0);
    interact_input use = {.use_click = 1, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    interact_out o = interact_frame(&s, &t.w, &t.ph, NULL, &p, &held, fx, eye, dir, hit, &use);
    CHECK(o.defib == 1 && o.actions == 0 && inv_count(&held, I_DEFIBRILLATOR) == 1);
    mem_free(fx);
    tw_free(&t);
}

void test_defib_all(void)
{
    test_rhythm_table();
    test_device_classifies();
    test_shock_other_rhythms();
    test_shock_outcomes();
    test_shock_beating_heart();
    test_device_normal_rhythm();
    test_device_vf();
    test_defib_item();
}
