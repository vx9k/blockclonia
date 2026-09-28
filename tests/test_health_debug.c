/* The debug injury table (health.h, HEALTH_DEBUG_KINDS) that --hurt and
 * the in-game debug menu both drive through health_injure: every entry
 * actually changes the body, the change survives a few seconds of the
 * model running (not just the instant after), and the table itself is
 * well formed. */
#include "mem.h"
#include "test_util.h"

#include <string.h>

static int any_wound(const health *h, int part, int kind)
{
    for (int i = 0; i < h->wound_count; i++)
        if (h->wounds[i].part == part && h->wounds[i].kind == kind) return 1;
    return 0;
}

/* The table itself: names are unique and non-empty, default parts and
 * part_kind are in range, --hurt's parser (health_debug_find) round-trips
 * every name. */
static void test_table_well_formed(void)
{
    CHECK(HEALTH_DEBUG_COUNT > 0);
    for (int i = 0; i < HEALTH_DEBUG_COUNT; i++) {
        const health_debug_kind *k = &HEALTH_DEBUG_KINDS[i];
        CHECK(k->name && k->name[0]);
        CHECK(k->label && k->label[0]);
        CHECK(k->default_part < BP_COUNT);
        CHECK(k->part_kind >= HDBG_ANY && k->part_kind <= HDBG_SYSTEMIC);
        if (!k->name) continue; /* already reported above; nothing else here is safe without it */
        CHECK(health_debug_find(k->name) == i);
        for (int j = i + 1; j < HEALTH_DEBUG_COUNT; j++)
            CHECK(strcmp(k->name, HEALTH_DEBUG_KINDS[j].name) != 0);
    }
    CHECK(health_debug_find("not-a-real-injury") == -1);
}

/* Applying an entry does nothing to a dead body, and an out-of-range
 * kind is ignored instead of reading past the table. */
static void test_injure_guards(void)
{
    health *h = new_body(1);
    health_env e = calm_env();
    health_injure(h, health_debug_find("vfib"), -1);
    live(h, &e, 90.0); /* past ARREST_DEATH_S: the body dies */
    CHECK(h->dead);
    float blood_at_death = h->blood;
    health_injure(h, health_debug_find("bleed"), BP_LLEG);
    CHECK(h->blood == blood_at_death); /* dead: no further injury */
    health_injure(h, -1, 0);
    health_injure(h, HEALTH_DEBUG_COUNT, 0); /* out of range: no crash */
    mem_free(h);
}

/* Each entry, applied through health_injure the way the menu and --hurt
 * do, leaves the specific state it names changed after the model has run
 * for a while -- not just in the instant after, which would miss a value
 * health_step reads back down (frostbite's freeze_time, pain's decay). */
static void test_each_kind_persists(void)
{
    for (int i = 0; i < HEALTH_DEBUG_COUNT; i++) {
        const health_debug_kind *k = &HEALTH_DEBUG_KINDS[i];
        health *h = new_body(100 + (uint32_t)i);
        health_env e = calm_env();
        /* Frostbite is meant to thaw in the warm room calm_env gives every
         * other entry, the way it would by a fire: check it over a couple
         * of seconds, long enough to prove health_injure set the state
         * step_frost actually reads (not just bp->frost on its own, which
         * would revert on the next step), short enough that it hasn't
         * thawed yet. */
        int frost_kind = !strcmp(k->name, "frostnip") || !strcmp(k->name, "frostbite") ||
                         !strcmp(k->name, "deep-frostbite");
        /* Rhythms that keep a pulse can stop or change by themselves (VT
         * turns pulseless or back to sinus): check them sooner. */
        int pulsed_rhythm = !strcmp(k->name, "afib") || !strcmp(k->name, "vtach") || !strcmp(k->name, "heart-block");
        double seconds = frost_kind || pulsed_rhythm ? 2.0 : 10.0;
        int part = k->part_kind == HDBG_ANY ? BP_CHEST : k->default_part;
        health_injure(h, i, part);
        int used_part = k->part_kind == HDBG_ANY ? BP_CHEST : k->default_part;
        live(h, &e, seconds);
        int ok = 0;
        /* Infection is a cut wound gone bad, so it leaves the same trace
         * as bleed/artery: one WOUND_CUT entry on the part. */
        if (!strcmp(k->name, "bleed") || !strcmp(k->name, "artery") || !strcmp(k->name, "infection"))
            ok = any_wound(h, used_part, WOUND_CUT);
        else if (!strcmp(k->name, "fracture") || !strcmp(k->name, "open-fracture"))
            ok = h->part[used_part].fracture != FX_NONE;
        else if (!strcmp(k->name, "concussion"))
            ok = h->concussion > 0.0f || h->confusion > 0.0f || h->part[BP_HEAD].integrity < 1.0f;
        else if (!strcmp(k->name, "burn") || !strcmp(k->name, "burn1") || !strcmp(k->name, "burn3"))
            ok = h->part[used_part].burn > 0;
        else if (!strcmp(k->name, "dislocation"))
            ok = h->part[used_part].dislocated != DISLOC_NONE;
        else if (!strcmp(k->name, "abrasion"))
            ok = any_wound(h, used_part, WOUND_ABRASION);
        else if (!strcmp(k->name, "crush")) ok = h->part[used_part].crush > 0.0f;
        else if (!strcmp(k->name, "frostnip") || !strcmp(k->name, "frostbite") || !strcmp(k->name, "deep-frostbite"))
            ok = h->part[used_part].frost != FROST_NONE;
        else if (!strcmp(k->name, "hypothermia"))
            ok = h->temp < 36.0f;
        else if (!strcmp(k->name, "hyperthermia"))
            ok = h->temp > 38.0f;
        else if (!strcmp(k->name, "pain"))
            ok = h->part[used_part].pain > 1.0f;
        else if (!strcmp(k->name, "shock"))
            ok = h->blood < BLOOD_NORMAL * 0.8f;
        else if (!strcmp(k->name, "sepsis"))
            ok = h->sepsis > 0.1f;
        else if (!strcmp(k->name, "vfib") || !strcmp(k->name, "asystole") || !strcmp(k->name, "pvt") ||
                 !strcmp(k->name, "pea"))
            ok = !health_rhythm_perfusing(h->rhythm);
        else if (pulsed_rhythm) ok = h->rhythm != RHYTHM_SINUS;
        else if (!strcmp(k->name, "crisis")) ok = h->catechol > 1.0f && h->surge > 0.0f && h->sbp > 160.0f;
        else if (!strcmp(k->name, "strain")) ok = h->strain > 5.0f;
        else
            ok = 0; /* an entry was added without teaching this test about it */
        CHECK(ok);
        mem_free(h);
    }
}

/* health_cut caps at HEALTH_MAX_WOUNDS; repeatedly triggering a
 * wound-adding entry must not overrun the array or corrupt another
 * wound's fields (the "infection" entry writes wounds[wound_count-1]). */
static void test_wound_cap(void)
{
    health *h = new_body(7);
    int bleed = health_debug_find("bleed"), infection = health_debug_find("infection");
    for (int i = 0; i < HEALTH_MAX_WOUNDS + 20; i++) {
        health_injure(h, bleed, i % BP_COUNT);
        health_injure(h, infection, i % BP_COUNT);
    }
    CHECK(h->wound_count >= 0 && h->wound_count <= HEALTH_MAX_WOUNDS);
    mem_free(h);
}

/* Field by field: body_part mixes uint8_t and float, and health mixes
 * float, int and double, so the compiler pads them; memcmp of the whole
 * struct compares that padding noise, not the state the game reads. */
static int part_equal(const body_part *a, const body_part *b)
{
    return a->integrity == b->integrity && a->fracture == b->fracture && a->splinted == b->splinted &&
           a->dislocated == b->dislocated && a->fracture_heal == b->fracture_heal && a->sprain == b->sprain &&
           a->internal == b->internal && a->pain == b->pain && a->pain_spike == b->pain_spike && a->skin == b->skin &&
           a->surface == b->surface && a->burn_omega == b->burn_omega && a->burn_area == b->burn_area &&
           a->burn_age == b->burn_age && a->cool_time == b->cool_time && a->burn == b->burn &&
           a->burn_cooled == b->burn_cooled && a->frost == b->frost && a->freeze_time == b->freeze_time &&
           a->frost_heal == b->frost_heal && a->crush == b->crush && a->crush_released == b->crush_released &&
           a->crush_load == b->crush_load && a->crush_next == b->crush_next;
}

static int wound_equal(const wound *a, const wound *b)
{
    return a->part == b->part && a->kind == b->kind && a->bandage == b->bandage && a->arterial == b->arterial &&
           a->depth == b->depth && a->bleed0 == b->bleed0 && a->bleed == b->bleed && a->clot == b->clot &&
           a->contamination == b->contamination && a->infection == b->infection && a->closure == b->closure &&
           a->bandage_age == b->bandage_age && a->area == b->area;
}

/* Float has representations (signalling NaN, -0.0 vs 0.0) that compare
 * equal or unequal by value without matching bit for bit, so an array of
 * floats is walked element by element rather than memcmp'd. */
static int farr_equal(const float *a, const float *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static int health_equal(const health *a, const health *b)
{
    for (int i = 0; i < BP_COUNT; i++)
        if (!part_equal(&a->part[i], &b->part[i])) return 0;
    if (!farr_equal(a->organ, b->organ, ORG_COUNT)) return 0;
    if (a->wound_count != b->wound_count) return 0;
    for (int i = 0; i < a->wound_count; i++)
        if (!wound_equal(&a->wounds[i], &b->wounds[i])) return 0;
    /* The log is a plain array of char, which has no representation quirk:
     * memcmp is a real comparison there. */
    if (!farr_equal(a->ecg, b->ecg, HEALTH_WAVE_LEN) || !farr_equal(a->art, b->art, HEALTH_WAVE_LEN) ||
        !farr_equal(a->pleth, b->pleth, HEALTH_WAVE_LEN) || !farr_equal(a->capno, b->capno, HEALTH_WAVE_LEN) ||
        memcmp(a->log, b->log, sizeof a->log) != 0 || !farr_equal(a->log_age, b->log_age, HEALTH_LOG))
        return 0;
    return a->blood == b->blood && a->rbc == b->rbc && a->water == b->water && a->stomach_water == b->stomach_water &&
           a->stomach_kcal == b->stomach_kcal && a->glycogen == b->glycogen && a->fat == b->fat &&
           a->bleed_ext == b->bleed_ext && a->bleed_int == b->bleed_int && a->hr == b->hr && a->sv == b->sv &&
           a->co == b->co && a->svr == b->svr && a->map == b->map && a->sbp == b->sbp && a->dbp == b->dbp &&
           a->symp == b->symp && a->ischemia == b->ischemia && a->strain == b->strain && a->catechol == b->catechol &&
           a->surge == b->surge && a->rr_k == b->rr_k && a->rhythm == b->rhythm && a->arrest_time == b->arrest_time &&
           a->beat_phase == b->beat_phase && a->pvc_pending == b->pvc_pending && a->last_beat_pvc == b->last_beat_pvc &&
           a->last_r == b->last_r && a->prev_r == b->prev_r && a->next_r == b->next_r && a->beat_pp == b->beat_pp &&
           a->beat_dbp == b->beat_dbp && a->rr == b->rr && a->breath_phase == b->breath_phase &&
           a->breathing == b->breathing && a->o2_store == b->o2_store && a->pao2 == b->pao2 && a->sao2 == b->sao2 &&
           a->spo2_shown == b->spo2_shown && a->paco2 == b->paco2 && a->etco2 == b->etco2 &&
           a->lung_water == b->lung_water && a->pneumothorax == b->pneumothorax && a->apnea_time == b->apnea_time &&
           a->gasp_timer == b->gasp_timer && a->power == b->power && a->vo2 == b->vo2 && a->wbal == b->wbal &&
           a->lactate == b->lactate && a->shock_debt == b->shock_debt && a->temp == b->temp && a->vaso == b->vaso &&
           a->sweat_w == b->sweat_w && a->sweat_acc == b->sweat_acc && a->wet == b->wet &&
           a->heat_loss == b->heat_loss && a->burn_tbsa == b->burn_tbsa && a->air_temp == b->air_temp &&
           a->radiant == b->radiant && a->in_fire == b->in_fire && a->reperfused == b->reperfused &&
           a->myoglobin == b->myoglobin && a->potassium == b->potassium && a->brain_o2 == b->brain_o2 &&
           a->ich == b->ich && a->ich_rate == b->ich_rate && a->icp == b->icp && a->concussion == b->concussion &&
           a->confusion == b->confusion && a->pain == b->pain && a->painkiller == b->painkiller &&
           a->antibiotic == b->antibiotic && a->sepsis == b->sepsis && a->conscious == b->conscious &&
           a->dead == b->dead && a->cause == b->cause && a->hurt_flash == b->hurt_flash && a->t == b->t &&
           a->rng == b->rng && a->wave_pos == b->wave_pos && a->wave_acc == b->wave_acc;
}

/* Determinism: the same sequence of debug injuries on two bodies seeded
 * and run identically ends in identical state (health's own replay
 * guarantee, exercised through this new entry point). */
static void test_replay(void)
{
    health *a = new_body(42), *b = new_body(42);
    health_env e = calm_env();
    for (int i = 0; i < HEALTH_DEBUG_COUNT; i++) {
        health_injure(a, i, BP_LARM);
        health_injure(b, i, BP_LARM);
    }
    live(a, &e, 5.0);
    live(b, &e, 5.0);
    CHECK(health_equal(a, b));
    mem_free(a);
    mem_free(b);
}

void test_health_debug_all(void)
{
    test_table_well_formed();
    test_injure_guards();
    test_each_kind_persists();
    test_wound_cap();
    test_replay();
}
