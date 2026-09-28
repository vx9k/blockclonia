/* The body in heat and cold, burns, frostbite, dislocations, abrasions and
 * crush injuries (health.c, survival.c), and how the HUD shows them. */
#include "hud.h"
#include "mem.h"
#include "survival.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

static float limb_skin(const health *h)
{
    return fmaxf(fmaxf(h->part[BP_LARM].skin, h->part[BP_RARM].skin),
                 fmaxf(h->part[BP_LLEG].skin, h->part[BP_RLEG].skin));
}

static int any_frost(const health *h)
{
    int f = 0;
    for (int i = 0; i < BP_COUNT; i++) f = h->part[i].frost > f ? h->part[i].frost : f;
    return f;
}

/* Still 20 C air in ordinary clothes is close to thermoneutral at rest:
 * the core holds 37 C for half an hour with the skin cooler toward the
 * hands and feet, as measured (trunk ~33-34 C, feet ~25-30 C). A hot room
 * opens the skin's vessels and brings out sweat. */
static void test_mild_air(void)
{
    health *h = new_body(21);
    health_env e = calm_env();
    live(h, &e, 1200.0);
    CHECK(fabsf(h->temp - 37.0f) < 0.3f);
    CHECK(h->part[BP_CHEST].skin > 31.0f && h->part[BP_CHEST].skin < 36.0f);
    CHECK(limb_skin(h) > 24.0f && limb_skin(h) < h->part[BP_CHEST].skin);
    /* Dry heat loss balances ~0.8 of resting metabolism (68 W); allow the
     * shells' slow drift. */
    CHECK(h->heat_loss > 40.0f && h->heat_loss < 110.0f);
    CHECK(any_frost(h) == 0 && h->wet == 0.0f && h->sweat_w < 1.0f);
    for (int p = 0; p < BP_COUNT; p++) CHECK(h->part[p].burn == 0);
    float vaso20 = h->vaso;

    health *w = new_body(22);
    health_env hot = calm_env();
    hot.air_temp = hot.contact_temp = 36.0;
    live(w, &hot, 1200.0);
    CHECK(w->vaso > vaso20 + 0.2f);
    CHECK(w->sweat_w > 20.0f);
    CHECK(fabsf(w->temp - 37.0f) < 0.5f); /* sweating holds it */
    CHECK(w->water < h->water);           /* and costs water */
    mem_free(h);
    mem_free(w);
}

/* -10 C with an 8 m/s wind (wind chill about -20 C) on light clothes and
 * bare hands: the hands and feet cool far faster than the trunk, and once
 * the rest of the skin is cold, cold-induced vasodilation stops and the
 * fingers freeze (frostbite in tens of minutes at this wind chill). The
 * same body warmed by a fire's radiant heat keeps its hands. */
static void test_cold(void)
{
    health *h = new_body(23), *f = new_body(23);
    health_env e = calm_env();
    e.air_temp = e.contact_temp = -10.0;
    e.wind = 8.0;
    health_env fire = e;
    fire.radiant = 1000.0; /* a campfire a metre or so away */
    live(h, &e, 600.0);
    float d_trunk = 33.5f - h->part[BP_CHEST].skin, d_hand = 31.0f - h->part[BP_RARM].skin,
          d_foot = 31.0f - h->part[BP_LLEG].skin;
    CHECK(d_hand > 2.0f * d_trunk && d_foot > 2.0f * d_trunk);
    CHECK(h->part[BP_RARM].skin < h->part[BP_HEAD].skin);
    CHECK(any_frost(h) == 0); /* not yet */
    char buf[160];
    health_part_status(h, BP_RARM, buf, sizeof buf);
    CHECK(strstr(buf, "numb with cold") != NULL);
    live(h, &e, 1500.0);
    live(f, &fire, 2100.0);
    CHECK(h->part[BP_LARM].frost >= FROST_SUPERFICIAL && h->part[BP_RARM].frost >= FROST_SUPERFICIAL);
    CHECK(h->part[BP_CHEST].frost == 0);
    CHECK(health_part_status(h, BP_RARM, buf, sizeof buf) >= 2 && strstr(buf, "frostbite") != NULL);
    CHECK(h->temp > 35.0f); /* shivering holds the core this long */
    CHECK(any_frost(f) == 0 && f->part[BP_RARM].skin > h->part[BP_RARM].skin + 5.0f);

    /* Frostnip clears when rewarmed by a fire. */
    health *n = new_body(24);
    live(n, &e, 1200.0);
    int nipped = n->part[BP_RARM].frost == FROST_NIP || n->part[BP_LARM].frost == FROST_NIP;
    CHECK(nipped);
    health_env warm = calm_env();
    warm.radiant = 800.0;
    live(n, &warm, 300.0);
    CHECK(n->part[BP_RARM].frost == 0 && n->part[BP_LARM].frost == 0);
    mem_free(h);
    mem_free(f);
    mem_free(n);
}

/* Stoll and Chianta (1969): bare skin under 20 kW/m^2 blisters (second
 * degree) in about 5 s, under 10 kW/m^2 in about 12-14 s, while 2 kW/m^2 is
 * near the long-exposure pain limit and does not burn quickly. Degrees
 * follow the Henriques integral's thresholds in order. */
static void burn_times(double flux, double t[4])
{
    health *h = new_body(25);
    health_env e = calm_env();
    e.radiant = flux;
    t[1] = t[2] = t[3] = -1.0;
    for (int i = 0; i < 60 * 60 && h->part[BP_HEAD].burn < 3; i++) {
        health_step(h, &e, 1.0 / 60.0);
        for (int k = 1; k <= h->part[BP_HEAD].burn && k <= 3; k++)
            if (t[k] < 0.0) t[k] = (i + 1) / 60.0;
    }
    mem_free(h);
}

static void test_burns(void)
{
    double a[4], b[4];
    burn_times(20000.0, a);
    burn_times(10000.0, b);
    CHECK(a[1] > 0.0 && a[1] <= a[2] && a[2] < a[3]);
    CHECK(a[2] > 3.0 && a[2] < 7.0);
    CHECK(a[3] < 15.0);
    CHECK(b[2] > 9.0 && b[2] < 17.0);

    health *h = new_body(26);
    health_env e = calm_env();
    e.radiant = 2000.0;
    live(h, &e, 60.0);
    for (int p = 0; p < BP_COUNT; p++) CHECK(h->part[p].burn == 0 && h->part[p].burn_omega < 0.1f);
    mem_free(h);

    /* The integral's thresholds through the API: 0.53 / 1 / 10^4. */
    h = new_body(27);
    health_burn(h, BP_LLEG, 0.3);
    CHECK(h->part[BP_LLEG].burn == 0);
    health_burn(h, BP_LLEG, 0.6);
    CHECK(h->part[BP_LLEG].burn == 1 && h->wound_count == 0);
    health_burn(h, BP_LLEG, 2.0);
    CHECK(h->part[BP_LLEG].burn == 2 && h->wound_count == 1 && h->wounds[0].kind == WOUND_BURN);
    health_burn(h, BP_LLEG, 1.0); /* a smaller dose does not heal it */
    CHECK(h->part[BP_LLEG].burn == 2);
    health_burn(h, BP_LLEG, 2e4);
    CHECK(h->part[BP_LLEG].burn == 3 && h->wound_count == 1 && h->part[BP_LLEG].integrity < 0.7f);
    char buf[160];
    CHECK(health_part_status(h, BP_LLEG, buf, sizeof buf) == 3);
    CHECK(strstr(buf, "3rd-degree burn, 9% of body") != NULL); /* half of the leg's 18% */
    mem_free(h);

    /* Flames climb from the feet: standing in a fire burns the legs first. */
    h = new_body(28);
    e = calm_env();
    e.in_fire = 1;
    live(h, &e, 3.0);
    CHECK(h->part[BP_LLEG].burn >= 2 && h->part[BP_HEAD].burn == 0 && h->part[BP_CHEST].burn == 0);
    CHECK(h->part[BP_LLEG].burn_area > 0.9f);
    mem_free(h);

    /* Deep burns over a third of the body leak plasma: the blood thickens
     * and the volume falls (burn shock). */
    h = new_body(29);
    e = calm_env();
    h->part[BP_LLEG].burn_area = h->part[BP_RLEG].burn_area = 1.0f;
    health_burn(h, BP_LLEG, 2e4);
    health_burn(h, BP_RLEG, 2e4);
    float hb0 = health_hb(h);
    live(h, &e, 300.0);
    CHECK(h->burn_tbsa > 30.0f);
    CHECK(h->blood < BLOOD_NORMAL - 1.0f && health_hb(h) > hb0 + 3.0f);
    CHECK(h->hr > 90.0f);
    mem_free(h);
}

/* Cooling a fresh burn under water takes the heat still stored in the skin
 * and stops the burn deepening over the next hours; left alone the same
 * burn keeps going. */
static void test_cool_burn(void)
{
    health *a = new_body(30), *b = new_body(30);
    health_env e = calm_env();
    e.radiant = 20000.0;
    live(a, &e, 5.2);
    live(b, &e, 5.2);
    CHECK(a->part[BP_HEAD].burn == 2);
    inventory inv;
    inv_init(&inv);
    char msg[128];
    e.radiant = 0.0;
    CHECK(health_treat(a, &inv, BP_HEAD, TREAT_COOL, 0, msg, sizeof msg) == 0); /* no water */
    health *c = new_body(30);
    CHECK(health_treat(c, &inv, BP_HEAD, TREAT_COOL, 1, msg, sizeof msg) == 0); /* not burned */
    mem_free(c);
    CHECK(health_treat(a, &inv, BP_HEAD, TREAT_COOL, 1, msg, sizeof msg) == 1);
    live(a, &e, 300.0);
    live(b, &e, 300.0);
    CHECK(a->part[BP_HEAD].burn_omega * 5.0f < b->part[BP_HEAD].burn_omega);
    char buf[160];
    health_part_status(a, BP_HEAD, buf, sizeof buf);
    CHECK(strstr(buf, "2nd-degree burn") != NULL && strstr(buf, "of body") != NULL);
    /* Dressing a burn: a bandage covers it. */
    inv_add(&inv, I_BANDAGE, 1);
    CHECK(health_treat(a, &inv, BP_HEAD, TREAT_BANDAGE, 0, msg, sizeof msg) == 1);
    CHECK(strstr(msg, "burn") != NULL);
    health_part_status(a, BP_HEAD, buf, sizeof buf);
    CHECK(strstr(buf, "(dressed)") != NULL);
    mem_free(a);
    mem_free(b);
}

/* Falls braced on the arms put shoulders out; a dislocated arm is useless
 * and very painful until it is reduced, which fails sometimes and never on
 * a broken limb. */
static void test_dislocation(void)
{
    int dislocated = 0, n = 40, reduced = 0, tries = 0;
    for (int s = 0; s < n; s++) {
        health *h = new_body(100 + (uint32_t)s);
        health_fall(h, 10.0, 1.0);
        int d = h->part[BP_LARM].dislocated || h->part[BP_RARM].dislocated;
        dislocated += d;
        if (d && !reduced) {
            int arm = h->part[BP_LARM].dislocated ? BP_LARM : BP_RARM;
            health_env e = calm_env();
            live(h, &e, 2.0);
            CHECK(h->part[arm].pain >= 7.0f);
            inventory inv;
            inv_init(&inv);
            char msg[128];
            if (h->part[arm].fracture) {
                CHECK(health_treat(h, &inv, arm, TREAT_REDUCE, 0, msg, sizeof msg) == 0);
            } else {
                while (h->part[arm].dislocated && tries < 12) {
                    CHECK(health_treat(h, &inv, arm, TREAT_REDUCE, 0, msg, sizeof msg) == 1);
                    tries++;
                }
                reduced = !h->part[arm].dislocated;
                CHECK(h->part[arm].pain_spike > 5.0f);
                CHECK(health_treat(h, &inv, arm, TREAT_REDUCE, 0, msg, sizeof msg) == 0); /* nothing left */
            }
        }
        mem_free(h);
    }
    CHECK(dislocated >= 3 && dislocated < n);
    CHECK(reduced && tries >= 1);
    /* A gentle landing does not. */
    health *g = new_body(7);
    health_fall(g, 5.0, 1.0);
    CHECK(!g->part[BP_LARM].dislocated && !g->part[BP_RARM].dislocated);

    /* Both arms out: the hands can't be used. */
    health_dislocate(g, BP_LARM);
    health_limits l = health_get_limits(g);
    CHECK(l.can_act);
    health_dislocate(g, BP_RARM);
    l = health_get_limits(g);
    CHECK(!l.can_act);
    char buf[160];
    CHECK(health_part_status(g, BP_RARM, buf, sizeof buf) >= 2 && strstr(buf, "dislocated shoulder") != NULL);
    health_dislocate(g, BP_LLEG); /* arms only */
    CHECK(!g->part[BP_LLEG].dislocated);
    /* Broken as well: splint it, don't reduce it. */
    health_break_bone(g, BP_RARM, 0);
    inventory inv;
    inv_init(&inv);
    char msg[128];
    CHECK(health_treat(g, &inv, BP_RARM, TREAT_REDUCE, 0, msg, sizeof msg) == 0);
    CHECK(g->part[BP_RARM].dislocated);
    mem_free(g);
}

/* Landing at speed on gravel-like ground: road rash on the legs and a
 * hand, shallow, wide, barely bleeding and dirty. A landing at walking
 * speed does nothing. */
static void test_abrasion(void)
{
    test_world t;
    tw_init(&t);
    player p;
    player_input idle = {0};
    health *h = new_body(31);
    for (int trial = 0; trial < 2; trial++) {
        player_spawn(&p, &t.w, 4.5, 4.5);
        p.pos.y = GROUND + 2.0;
        p.vel = dv3(trial ? 12.0 : 3.0, 0.0, 0.0);
        p.on_ground = 0;
        for (int i = 0; i < 90; i++) {
            survival_before b = survival_capture(&p);
            physics_step(&t.ph, &p, &idle);
            survival_impacts(h, &t.w, &t.ph, &p, &b);
        }
        CHECK(p.on_ground);
        if (!trial) CHECK(h->wound_count == 0);
    }
    int legs = 0;
    for (int i = 0; i < h->wound_count; i++) {
        const wound *w = &h->wounds[i];
        CHECK(w->kind == WOUND_ABRASION);
        CHECK(w->depth < 0.3f && w->area > 0.1f && w->bleed0 < 15.0f && w->contamination > 0.4f);
        legs += w->part == BP_LLEG || w->part == BP_RLEG;
    }
    CHECK(legs == 2 && h->wound_count >= 3);
    char buf[160];
    health_part_status(h, BP_LLEG, buf, sizeof buf);
    CHECK(strstr(buf, "abrasion") != NULL);
    /* Ooze clots within minutes; the grit makes it infect if not cleaned. */
    health_env e = calm_env();
    live(h, &e, 300.0);
    CHECK(h->bleed_ext < 3.0f);

    health *k = new_body(32);
    health_abrasion(k, BP_LARM, 0.3f, 0.8f);
    CHECK(k->wound_count == 1 && fabsf(k->wounds[0].area - 0.3f) < 1e-6f);
    mem_free(k);
    mem_free(h);
    tw_free(&t);
}

/* A leg under 400 kg for two game hours (100 s): the muscle dies. Freed,
 * it floods the blood with myoglobin and potassium and the kidneys suffer;
 * a brief pinning does not. A weight on the chest stops breathing. */
static void test_crush(void)
{
    health *a = new_body(33), *b = new_body(33);
    health_env e = calm_env();
    for (int i = 0; i < 120 * 60; i++) {
        health_crush(a, BP_LLEG, 400.0, 1.0 / 60.0);
        if (i < 10 * 60) health_crush(b, BP_LLEG, 400.0, 1.0 / 60.0);
        health_step(a, &e, 1.0 / 60.0);
        health_step(b, &e, 1.0 / 60.0);
    }
    CHECK(a->part[BP_LLEG].crush > 0.9f && b->part[BP_LLEG].crush < 0.15f);
    CHECK(a->part[BP_LLEG].crush_load > 399.0f && b->part[BP_LLEG].crush_load == 0.0f);
    char buf[160];
    CHECK(health_part_status(a, BP_LLEG, buf, sizeof buf) >= 2 && strstr(buf, "pinned under 400 kg") != NULL);
    CHECK(a->organ[ORG_KIDNEYS] > 0.99f); /* nothing reaches the kidneys while it is still trapped */
    float kmax = 0.0f, myo = 0.0f;
    for (int s = 0; s < 300; s++) {
        live(a, &e, 1.0);
        live(b, &e, 1.0);
        kmax = fmaxf(kmax, a->potassium);
        myo = fmaxf(myo, a->myoglobin);
    }
    CHECK(myo > 10.0f && kmax > 6.0f);
    CHECK(a->organ[ORG_KIDNEYS] < 0.9f);
    CHECK(b->organ[ORG_KIDNEYS] > a->organ[ORG_KIDNEYS] + 0.1f);
    health_part_status(a, BP_LLEG, buf, sizeof buf);
    CHECK(strstr(buf, "crushed muscle") != NULL);
    CHECK(health_organ_status(a, ORG_KIDNEYS, buf, sizeof buf) >= 2);

    /* 250 kg on the chest: no breath; CO2 builds and the oxygen store runs
     * down as in breath-holding (about 90% after 2.5 minutes at rest). */
    health *c = new_body(34);
    for (int i = 0; i < 150 * 60; i++) {
        health_crush(c, BP_CHEST, 250.0, 1.0 / 60.0);
        health_step(c, &e, 1.0 / 60.0);
    }
    CHECK(!c->breathing && c->sao2 < 0.93f && c->paco2 > 55.0f);
    live(c, &e, 60.0);
    CHECK(c->breathing && c->sao2 > 0.9f);
    mem_free(a);
    mem_free(b);
    mem_free(c);

    /* The physics' pinned weight reaches the legs through survival_impacts. */
    test_world t;
    tw_init(&t);
    player p;
    player_spawn(&p, &t.w, 4.5, 4.5);
    health *h = new_body(35);
    survival_before sb = survival_capture(&p);
    t.ph.pinned_mass = 300.0f;
    t.ph.pinned_height = 0.2f;
    survival_impacts(h, &t.w, &t.ph, &p, &sb);
    health_step(h, &e, PHYS_DT);
    CHECK(fabsf(h->part[BP_LLEG].crush_load - 150.0f) < 1e-3f && fabsf(h->part[BP_RLEG].crush_load - 150.0f) < 1e-3f);
    t.ph.pinned_mass = 0.0f;
    survival_impacts(h, &t.w, &t.ph, &p, &sb);
    health_step(h, &e, PHYS_DT);
    CHECK(h->part[BP_LLEG].crush_load == 0.0f);

    /* The environment: mild air until thermo says otherwise, and standing
     * on a burning campfire is standing in flames. */
    health_env env;
    survival_env(&t.w, &p, &sb, 0, &env);
    CHECK(env.air_temp == SURVIVAL_AIR_TEMP && env.radiant == 0.0 && !env.in_fire);
    survival_env_thermal(&env, -4.0, 1200.0, -2.0, 0);
    CHECK(env.air_temp == -4.0 && env.radiant == 1200.0 && env.contact_temp == -2.0 && !env.in_fire);
    world_set(&t.w, 4, GROUND, 4, B_CAMPFIRE, 4);
    player_spawn(&p, &t.w, 4.5, 4.5);
    survival_env(&t.w, &p, &sb, 0, &env);
    CHECK(env.in_fire);
    mem_free(h);
    tw_free(&t);
}

/* Soaked clothes dry by evaporation: in front of a fire within a minute or
 * two (game hours), in still freezing air far slower, and while wet they
 * chill the skin. */
static void test_wetness(void)
{
    health *a = new_body(36), *b = new_body(36);
    health_env e = calm_env();
    e.submerged = 1.0;
    e.airway = AIRWAY_WATER;
    live(a, &e, 8.0);
    live(b, &e, 8.0);
    CHECK(a->wet > 0.95f);
    health_env fire = calm_env(), cold = calm_env();
    fire.air_temp = fire.contact_temp = 10.0;
    fire.radiant = 1500.0;
    cold.air_temp = cold.contact_temp = -5.0;
    live(a, &fire, 60.0);
    live(b, &cold, 60.0);
    CHECK(a->wet < b->wet - 0.3f);
    CHECK(b->wet > 0.6f);
    live(a, &fire, 120.0);
    CHECK(a->wet == 0.0f);
    mem_free(a);
    mem_free(b);
}

static int has_alert(const hud_state *s, const char *text)
{
    for (int i = 0; i < s->alert_count; i++)
        if (s->alerts[i].live && strstr(s->alerts[i].text, text)) return 1;
    return 0;
}

/* The HUD names the new injuries, animates alerts in and out, and the
 * screens still fit the renderer's quad buffer. */
static void test_hud(void)
{
    enum { MAXQ = 8192 };
    ui_vertex *mem = mem_alloc(sizeof(ui_vertex) * 4 * MAXQ);
    ui u;
    health *h = new_body(37);
    health_env e = calm_env();
    health_burn(h, BP_HEAD, 3.0);
    health_dislocate(h, BP_LARM);
    h->part[BP_RLEG].frost = FROST_SUPERFICIAL;
    health_abrasion(h, BP_LLEG, 0.3f, 0.7f);
    for (int i = 0; i < 60; i++) {
        health_crush(h, BP_RARM, 120.0, 1.0 / 60.0);
        health_step(h, &e, 1.0 / 60.0);
    }
    hud_state s;
    memset(&s, 0, sizeof s);
    s.held = "stone";
    hud_update(&s, h, 1.0f / 60.0f);
    CHECK(has_alert(&s, "Burned: 2nd degree") && has_alert(&s, "Dislocated left shoulder"));
    CHECK(has_alert(&s, "Frostbite") && has_alert(&s, "Crushed: pinned under 120 kg"));
    /* New alerts start off to the left and slide in. */
    CHECK(s.alert_count > 0 && s.alerts[0].slide > 0.5f);
    for (int i = 0; i < 60; i++) hud_update(&s, h, 1.0f / 60.0f);
    CHECK(s.alerts[0].slide == 0.0f && s.alerts[0].alpha > 0.99f);
    /* Reduced: the alert fades out rather than vanishing. */
    h->part[BP_LARM].dislocated = 0;
    hud_update(&s, h, 1.0f / 60.0f);
    int fading = 0;
    for (int i = 0; i < s.alert_count; i++) fading |= !s.alerts[i].live && strstr(s.alerts[i].text, "Dislocated");
    CHECK(fading && !has_alert(&s, "Dislocated"));
    for (int i = 0; i < 60; i++) hud_update(&s, h, 1.0f / 60.0f);
    fading = 0;
    for (int i = 0; i < s.alert_count; i++) fading |= strstr(s.alerts[i].text, "Dislocated") != NULL;
    CHECK(!fading);

    ui_begin(&u, mem, MAXQ, 1280, 720);
    hud_draw(&u, h, &s);
    CHECK(u.overflow == 0 && u.quads > 200);
    s.panel = 1;
    for (s.sel = 0; s.sel < BP_COUNT; s.sel++) {
        ui_begin(&u, mem, MAXQ, 1280, 720);
        hud_draw(&u, h, &s);
        CHECK(u.overflow == 0 && u.quads > 1000);
    }
    char buf[160];
    health_part_status(h, BP_RLEG, buf, sizeof buf);
    CHECK(strstr(buf, "frostbite") != NULL);
    health_part_status(h, BP_RARM, buf, sizeof buf);
    CHECK(strstr(buf, "pinned under 120 kg") != NULL);

    /* Soaked in cold air, and a crushed limb once freed. */
    health *w = new_body(38);
    health_env wet = calm_env();
    wet.submerged = 1.0;
    live(w, &wet, 8.0);
    health_env cold = calm_env();
    cold.air_temp = cold.contact_temp = 2.0;
    w->part[BP_LLEG].crush = 0.5f;
    live(w, &cold, 1.0);
    hud_state s2;
    memset(&s2, 0, sizeof s2);
    hud_update(&s2, w, 0.1f);
    CHECK(has_alert(&s2, "Soaked and freezing") && has_alert(&s2, "Crushed Left leg"));
    ui_begin(&u, mem, MAXQ, 1280, 720);
    hud_draw(&u, w, &s2);
    CHECK(u.overflow == 0);
    mem_free(w);
    mem_free(h);
    mem_free(mem);
}

/* -------------------------------------------------------------- variance */

static health *varied_body(uint32_t seed)
{
    health *h = mem_alloc(sizeof *h);
    health_init_varied(h, seed);
    return h;
}

/* health_init (what new_body uses) must stay the neutral reference body:
 * the mean of every varied parameter, and no short-term noise, whatever
 * seed it's given. Everything else in this file checks exact numbers
 * against it, so this is what keeps that meaningful. */
static void test_variance_neutral(void)
{
    for (uint32_t seed = 1; seed <= 5; seed++) {
        health *h = new_body(seed * 97 + 3);
        CHECK(h->base.vary == 0);
        CHECK(h->base.hr_rest == 64.0f && h->base.map_set == 93.0f);
        CHECK(h->base.pain_gain == 1.0f && h->base.clot_gain == 1.0f && h->base.metab_gain == 1.0f);
        CHECK(h->base.cold_bias == 0.0f);
        mem_free(h);
    }
}

/* Checks the state that matters for replay: the constitution, the fluid,
 * circulatory, respiratory and metabolic scalars, both rng streams and
 * the wound the test inflicts. Not a memcmp of the whole struct: that
 * compares padding bytes too, which need not match. */
static int bodies_match(const health *a, const health *b)
{
    const health_baseline *x = &a->base, *y = &b->base;
    if (x->hr_rest != y->hr_rest || x->map_set != y->map_set || x->pain_gain != y->pain_gain) return 0;
    if (x->clot_gain != y->clot_gain || x->metab_gain != y->metab_gain || x->cold_bias != y->cold_bias) return 0;
    if (x->vary != y->vary) return 0;
    if (a->rng != b->rng || a->vrng != b->vrng) return 0;
    if (a->hr != b->hr || a->map != b->map || a->sbp != b->sbp || a->dbp != b->dbp) return 0;
    if (a->rr != b->rr || a->sao2 != b->sao2 || a->spo2_shown != b->spo2_shown) return 0;
    if (a->temp != b->temp || a->blood != b->blood || a->water != b->water || a->power != b->power) return 0;
    if (a->wound_count != b->wound_count) return 0;
    for (int i = 0; i < a->wound_count; i++)
        if (a->wounds[i].bleed != b->wounds[i].bleed || a->wounds[i].clot != b->wounds[i].clot) return 0;
    return 1;
}

/* A varied body's constitution and its whole trajectory (including the
 * short-term noise) must replay exactly from the seed alone. */
static void test_variance_replay(void)
{
    health *a = varied_body(4242), *b = varied_body(4242);
    CHECK(bodies_match(a, b));
    health_env e = calm_env();
    for (int i = 0; i < 3; i++) {
        live(a, &e, 5.0);
        live(b, &e, 5.0);
        health_cut(a, BP_LARM, 0.4f, 0, 0.1f);
        health_cut(b, BP_LARM, 0.4f, 0, 0.1f);
    }
    CHECK(bodies_match(a, b));
    mem_free(a);
    mem_free(b);
}

/* Different seeds draw different, but still physiological, constitutions,
 * and a sweep of seeds spreads across most of each range: a body seeded
 * unhashed would cluster near one end (small seeds give a near-zero first
 * draw from the xorshift rng). */
static void test_variance_range(void)
{
    float hr_lo = 1e9f, hr_hi = -1e9f, map_lo = 1e9f, map_hi = -1e9f;
    for (uint32_t seed = 1; seed <= 200; seed++) {
        health *h = varied_body(seed);
        CHECK(h->base.vary == 1);
        CHECK(h->base.hr_rest >= 55.0f && h->base.hr_rest <= 85.0f);
        CHECK(h->base.map_set >= 85.0f && h->base.map_set <= 101.0f);
        CHECK(h->base.pain_gain >= 0.8f && h->base.pain_gain <= 1.2f);
        CHECK(h->base.clot_gain >= 0.75f && h->base.clot_gain <= 1.35f);
        CHECK(h->base.metab_gain >= 0.92f && h->base.metab_gain <= 1.1f);
        CHECK(h->base.cold_bias >= -0.4f && h->base.cold_bias <= 0.4f);
        hr_lo = fminf(hr_lo, h->base.hr_rest);
        hr_hi = fmaxf(hr_hi, h->base.hr_rest);
        map_lo = fminf(map_lo, h->base.map_set);
        map_hi = fmaxf(map_hi, h->base.map_set);
        mem_free(h);
    }
    CHECK(hr_hi - hr_lo > 25.0f);   /* covers most of the 30 bpm range */
    CHECK(map_hi - map_lo > 13.0f); /* covers most of the 16 mmHg range */
}

/* At rest, the emergent heart rate, pressure, saturation and core
 * temperature stay physiological across seeds, and two seeds with very
 * different resting heart rates settle at visibly different heart rates:
 * the baroreflex doesn't erase the difference. */
static void test_variance_emergent(void)
{
    health *lo = NULL, *hi = NULL;
    float lo_hr = 1e9f, hi_hr = -1e9f;
    /* 60 s is plenty: the circulatory lags settle in a handful of seconds
     * (baroreflex tau 4 s, heart rate tau 2.5-4 s, MAP tau 1 s). */
    for (uint32_t seed = 1; seed <= 16; seed++) {
        health *h = varied_body(seed);
        health_env e = calm_env();
        live(h, &e, 60.0);
        CHECK(h->hr > 45.0f && h->hr < 110.0f);
        CHECK(h->map > 65.0f && h->map < 115.0f);
        CHECK(h->sao2 > 0.94f);
        CHECK(fabsf(h->temp - 37.0f) < 0.5f);
        if (h->base.hr_rest < (lo ? lo->base.hr_rest : 1e9f)) {
            if (lo) mem_free(lo);
            lo = h;
            lo_hr = h->hr;
        } else if (h->base.hr_rest > (hi ? hi->base.hr_rest : -1e9f)) {
            if (hi) mem_free(hi);
            hi = h;
            hi_hr = h->hr;
        } else {
            mem_free(h);
        }
    }
    CHECK(hi_hr - lo_hr > 5.0f);
    mem_free(lo);
    mem_free(hi);
}

/* Short-term variability wanders both ways around the baseline and stays
 * bounded; a neutral body (health_init) shows none of it. */
static void test_variance_shortterm(void)
{
    health *h = varied_body(99);
    health_env e = calm_env();
    live(h, &e, 30.0); /* let the baroreflex settle first */
    float lo = 1e9f, hi = -1e9f, first = h->hr;
    int moved = 0;
    for (int i = 0; i < 600; i++) {
        health_step(h, &e, 1.0 / 60.0);
        lo = fminf(lo, h->hr);
        hi = fmaxf(hi, h->hr);
        if (fabsf(h->hr - first) > 0.05f) moved = 1;
    }
    CHECK(moved);                     /* the HUD doesn't look frozen */
    CHECK(hi - lo < 15.0f);           /* bounded: no runaway wander */
    CHECK(lo > h->base.hr_rest - 30.0f && hi < h->base.hr_rest + 60.0f);

    health *ref = new_body(99);
    live(ref, &e, 30.0);
    float rhr0 = ref->hr, ref_lo = rhr0, ref_hi = rhr0;
    for (int i = 0; i < 600; i++) {
        health_step(ref, &e, 1.0 / 60.0);
        ref_lo = fminf(ref_lo, ref->hr);
        ref_hi = fmaxf(ref_hi, ref->hr);
    }
    /* Settled, it only drifts by the lag's residual convergence, nowhere
     * near the varied body's beat-to-beat wander. */
    CHECK(ref_hi - ref_lo < 0.05f);
    mem_free(h);
    mem_free(ref);
}

void test_health2_all(void)
{
    test_mild_air();
    test_cold();
    test_burns();
    test_cool_burn();
    test_dislocation();
    test_abrasion();
    test_crush();
    test_wetness();
    test_hud();
    test_variance_neutral();
    test_variance_replay();
    test_variance_range();
    test_variance_emergent();
    test_variance_shortterm();
}
