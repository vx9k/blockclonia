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
        double seconds = frost_kind ? 2.0 : 10.0;
        int part = k->part_kind == HDBG_ANY ? BP_CHEST : k->default_part;
        health_injure(h, i, part);
        int used_part = k->part_kind == HDBG_ANY ? BP_CHEST : k->default_part;
        live(h, &e, seconds);
        int ok = 0;
        if (!strcmp(k->name, "bleed") || !strcmp(k->name, "artery"))
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
        else if (!strcmp(k->name, "crush"))
            ok = h->part[used_part].crush > 0.0f;
        else if (!strcmp(k->name, "infection"))
            ok = any_wound(h, used_part, WOUND_CUT);
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
        else if (!strcmp(k->name, "vfib") || !strcmp(k->name, "asystole"))
            ok = h->rhythm != RHYTHM_SINUS;
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
    CHECK(memcmp(a, b, sizeof *a) == 0);
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
