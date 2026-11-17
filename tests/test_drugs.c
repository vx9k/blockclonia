/* Morphine, naloxone and alcohol: the items, how they are absorbed and
 * cleared, what they do to pain, breathing, heat and the liver, and how
 * they act together. */
#include "health.h"
#include "inventory.h"
#include "item.h"
#include "mem.h"
#include "test_util.h"
#include "thoughts.h"

#include <string.h>

/* Real seconds in one game hour of the survival clock. */
#define GAME_H (3600.0 / HEALTH_CLOCK)

static int treat(health *h, inventory *inv, int what)
{
    char msg[128] = {0};
    return health_treat(h, inv, BP_CHEST, what, 0, msg, sizeof msg);
}

static void stock(inventory *inv, item_id id, int n)
{
    inv_init(inv);
    inv_add(inv, id, n);
}

static void test_drug_items(void)
{
    CHECK(I_SPIRITS + 1 == I_TOOL_FIRST && I_TOOL_END == I_RAW_TIN && I_LIQUIDS_END == ITEM_END);
    CHECK(item_valid(I_MORPHINE) && item_valid(I_NALOXONE) && item_valid(I_SPIRITS) && !item_valid(ITEM_END));
    int kind = 0, tier = 0;
    CHECK(!item_tool(I_MORPHINE, &kind, &tier) && !item_tool(I_SPIRITS, &kind, &tier));
    CHECK(item_tool(I_TOOL_END - 1, &kind, &tier) && tier == TIER_COUNT - 1);
    CHECK(!strcmp(item_get(I_MORPHINE)->name, "Morphine") && item_get(I_SPIRITS)->tex == T_ITEM_SPIRITS);
    CHECK(item_get(I_NALOXONE)->stack == 4 && item_get(I_NALOXONE)->block == B_AIR);

    inventory inv = {0};
    inv_starting_kit(&inv);
    CHECK(inv_count(&inv, I_MORPHINE) == 2 && inv_count(&inv, I_NALOXONE) == 1 && inv_count(&inv, I_SPIRITS) == 0);
    uint8_t buf[INV_ENCODED_SIZE] = {0};
    CHECK(inv_encode(&inv, buf, sizeof buf) == INV_ENCODED_SIZE);
    inventory back = {0};
    CHECK(inv_decode(&back, buf, sizeof buf) == 0 && inv_count(&back, I_MORPHINE) == 2);
    CHECK(inv_count(&back, I_NALOXONE) == 1);

    int found = 0;
    for (int i = 0; i < g_recipe_count; i++)
        if (g_recipes[i].out.id == I_SPIRITS)
            found = g_recipes[i].station == STATION_FIRE && g_recipes[i].in[0].id == I_APPLE;
    CHECK(found);
}

static void test_out_of_stock(void)
{
    health *h = new_body(70);
    inventory none = {0};
    inv_init(&none);
    CHECK(!treat(h, &none, TREAT_MORPHINE) && !treat(h, &none, TREAT_NALOXONE) && !treat(h, &none, TREAT_SPIRITS));
    CHECK(h->opioid_gut == 0.0f && h->naloxone == 0.0f && h->alcohol_gut == 0.0f);
    inventory inv = {0};
    stock(&inv, I_SPIRITS, 2);
    h->stomach_water = 2.0f;
    CHECK(!treat(h, &inv, TREAT_SPIRITS) && inv_count(&inv, I_SPIRITS) == 2);
    mem_free(h);
}

/* One tablet: absorbed in about half an hour, gone with a 3 h half-life,
 * and the pain it takes away without slowing breathing much. */
static void test_morphine_kinetics(void)
{
    health *h = new_body(71), *ref = new_body(71);
    health_env e = calm_env();
    inventory inv = {0};
    stock(&inv, I_MORPHINE, 4);
    h->part[BP_LLEG].pain_spike = ref->part[BP_LLEG].pain_spike = 6.0f;
    CHECK(treat(h, &inv, TREAT_MORPHINE) && inv_count(&inv, I_MORPHINE) == 3);
    CHECK(h->opioid_gut == 10.0f && h->opioid == 0.0f);
    live(h, &e, GAME_H);
    live(ref, &e, GAME_H);
    CHECK(h->opioid_gut < 2.0f && h->opioid > 6.0f && h->opioid < 10.0f);
    CHECK(h->pain < ref->pain * 0.75f);
    CHECK(health_resp_depression(h) < 0.1f && h->rr > 10.0f);
    h->opioid_gut = 0.0f;
    h->opioid = 10.0f;
    live(h, &e, 3.0 * GAME_H);
    CHECK(h->opioid > 4.5f && h->opioid < 5.5f);
    mem_free(h);
    mem_free(ref);
}

/* Six tablets at once slow breathing to a dangerous crawl; naloxone brings
 * it back, but wears off before the morphine does. */
static void test_overdose_and_naloxone(void)
{
    health *h = new_body(72);
    health_env e = calm_env();
    inventory inv = {0};
    stock(&inv, I_MORPHINE, 6);
    for (int i = 0; i < 6; i++) CHECK(treat(h, &inv, TREAT_MORPHINE));
    live(h, &e, 1.5 * GAME_H);
    CHECK(h->opioid > 40.0f);
    CHECK(health_resp_depression(h) > 0.6f && h->rr < 10.0f);
    CHECK(health_sedation(h) > 0.45f && h->conscious != CONS_ALERT);

    inventory spray = {0};
    stock(&spray, I_NALOXONE, 1);
    CHECK(treat(h, &spray, TREAT_NALOXONE) && h->naloxone > 0.9f && inv_count(&spray, I_NALOXONE) == 0);
    CHECK(health_resp_depression(h) < 0.05f && h->surge > 0.0f);
    live(h, &e, 30.0);
    CHECK(h->rr > 12.0f);
    live(h, &e, 1.2 * GAME_H);
    CHECK(h->naloxone == 0.0f && h->opioid > 20.0f && health_resp_depression(h) > 0.3f);
    mem_free(h);
}

static void test_overdose_kills(void)
{
    health *h = new_body(73);
    health_env e = calm_env();
    h->opioid = 120.0f;
    for (int i = 0; i < 40 && !h->dead; i++) live(h, &e, 30.0);
    CHECK(h->dead && h->cause == DEATH_OVERDOSE);
    CHECK(!strcmp(health_death_text(DEATH_OVERDOSE), "Overdose: breathing stopped"));
    mem_free(h);
}

/* Widmark: four drinks in a 75 kg body give about 1.1 g/L, cleared at
 * ~0.15 g/L an hour; a meal slows the rise. */
static void test_alcohol_kinetics(void)
{
    health *h = new_body(74), *fed = new_body(74);
    health_env e = calm_env();
    inventory inv = {0}, inv2 = {0};
    stock(&inv, I_SPIRITS, 4);
    stock(&inv2, I_SPIRITS, 4);
    h->stomach_kcal = 0.0f;
    fed->stomach_kcal = 600.0f;
    for (int i = 0; i < 4; i++) {
        CHECK(treat(h, &inv, TREAT_SPIRITS));
        CHECK(treat(fed, &inv2, TREAT_SPIRITS));
    }
    CHECK(inv_count(&inv, I_SPIRITS) == 0 && h->alcohol_gut == 56.0f);
    live(h, &e, 0.25 * GAME_H);
    live(fed, &e, 0.25 * GAME_H);
    CHECK(fed->bac < h->bac * 0.8f);
    live(h, &e, 2.0 * GAME_H);
    CHECK(h->bac > 0.6f && h->bac < 1.2f && h->alcohol_gut < 1.0f);
    CHECK(h->drink_load > 40.0f);
    float before = h->bac;
    live(h, &e, 2.0 * GAME_H);
    CHECK(before - h->bac > 0.2f && before - h->bac < 0.4f);
    live(h, &e, 8.0 * GAME_H);
    CHECK(h->bac == 0.0f);
    mem_free(h);
    mem_free(fed);
}

/* Drink cuts the dose of morphine that stops breathing. */
static void test_interaction(void)
{
    health *h = new_body(75);
    h->opioid = 20.0f;
    float sober = health_resp_depression(h);
    h->bac = 1.5f;
    float drunk = health_resp_depression(h);
    CHECK(drunk > sober * 2.0f);
    h->opioid = 0.0f;
    CHECK(health_resp_depression(h) == 0.0f);
    h->bac = 4.0f;
    CHECK(health_resp_depression(h) > 0.4f && health_sedation(h) > 0.85f);
    h->naloxone = 1.0f; /* naloxone does nothing for alcohol */
    CHECK(health_resp_depression(h) > 0.4f);
    mem_free(h);
}

/* The warm glow is heat leaving: drunk in the cold, the core cools faster. */
static void test_alcohol_cold(void)
{
    health *h = new_body(76), *ref = new_body(76);
    health_env e = calm_env();
    e.air_temp = -5.0;
    h->bac = 1.5f;
    live(h, &e, 60.0);
    live(ref, &e, 60.0);
    CHECK(h->vaso > ref->vaso + 0.4f);
    CHECK(h->temp < ref->temp);
    mem_free(h);
    mem_free(ref);
}

/* A drunk lying unconscious with a full stomach vomits and breathes it in. */
static void test_aspiration(void)
{
    health *h = new_body(77);
    health_env e = calm_env();
    h->bac = 3.6f;
    h->stomach_water = 0.6f;
    h->stomach_kcal = 400.0f;
    float lungs = h->organ[ORG_LUNGS];
    for (int i = 0; i < 60 && h->lung_water < 0.05f; i++) live(h, &e, 2.0);
    CHECK(h->conscious == CONS_UNCONSCIOUS);
    CHECK(h->lung_water >= 0.1f && h->organ[ORG_LUNGS] < lungs);
    mem_free(h);
}

/* Days of heavy drinking halve the painkiller the liver takes. */
static void test_painkiller_with_drink(void)
{
    health *h = new_body(78), *ref = new_body(78);
    inventory inv = {0}, inv2 = {0};
    stock(&inv, I_PAINKILLER, 3);
    stock(&inv2, I_PAINKILLER, 3);
    h->drink_load = 250.0f;
    for (int i = 0; i < 3; i++) {
        CHECK(treat(h, &inv, TREAT_PAINKILLER));
        CHECK(treat(ref, &inv2, TREAT_PAINKILLER));
    }
    CHECK(h->hepatotox > 0.0f && ref->hepatotox == 0.0f);
    mem_free(h);
    mem_free(ref);
}

/* Without antiseptic, spirits clean a wound before water would. */
static void test_spirits_disinfect(void)
{
    health *h = new_body(79);
    health_cut(h, BP_LARM, 0.4f, 0, 0.8f);
    inventory inv = {0};
    stock(&inv, I_SPIRITS, 1);
    char msg[128] = {0};
    CHECK(health_treat(h, &inv, BP_LARM, TREAT_DISINFECT, 1, msg, sizeof msg));
    CHECK(inv_count(&inv, I_SPIRITS) == 0 && strstr(msg, "spirits") != NULL);
    CHECK(h->bac == 0.0f && h->alcohol_gut == 0.0f);
    mem_free(h);
}

/* Same seed, same doses: the same body, to the bit. */
static void test_drug_replay(void)
{
    health *a = new_body(80), *b = new_body(80);
    health_env e = calm_env();
    a->bac = b->bac = 3.2f;
    a->opioid = b->opioid = 25.0f;
    a->stomach_water = b->stomach_water = 0.5f;
    live(a, &e, 300.0);
    live(b, &e, 300.0);
    CHECK(a->bac == b->bac && a->opioid == b->opioid && a->lung_water == b->lung_water && a->rr == b->rr);
    mem_free(a);
    mem_free(b);
}

static void test_drug_feelings(void)
{
    health *h = new_body(81);
    CHECK(thoughts_level(h, FEEL_DRUNK) == 0 && thoughts_level(h, FEEL_DOPED) == 0);
    h->bac = 1.5f;
    CHECK(thoughts_level(h, FEEL_DRUNK) == 2);
    h->opioid = 20.0f;
    CHECK(thoughts_level(h, FEEL_DOPED) == 2);
    h->naloxone = 1.0f;
    CHECK(thoughts_level(h, FEEL_DOPED) == 0);
    for (int f = FEEL_DRUNK; f <= FEEL_DOPED; f++)
        for (int l = 0; l < 3; l++) CHECK(thoughts_lines(f, l) > 0);
    mem_free(h);
}

/* The messages warn about the dangerous doses and say when there is
 * nothing to reverse. */
static void test_drug_messages(void)
{
    health *h = new_body(82);
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, I_MORPHINE, 3);
    inv_add(&inv, I_NALOXONE, 2);
    char msg[128] = {0};
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_NALOXONE, 0, msg, sizeof msg) && strstr(msg, "no opioid"));
    CHECK(h->surge == 0.0f);
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_MORPHINE, 0, msg, sizeof msg) && strstr(msg, "half an hour"));
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_MORPHINE, 0, msg, sizeof msg) && strstr(msg, "half an hour"));
    h->bac = 1.0f;
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_MORPHINE, 0, msg, sizeof msg) && strstr(msg, "on top of drink"));
    h->bac = 2.5f;
    inventory drink = {0};
    stock(&drink, I_SPIRITS, 1);
    CHECK(health_treat(h, &drink, BP_CHEST, TREAT_SPIRITS, 0, msg, sizeof msg) && strstr(msg, "far too much"));
    mem_free(h);
}

void test_drugs_all(void)
{
    test_drug_messages();
    test_drug_items();
    test_out_of_stock();
    test_morphine_kinetics();
    test_overdose_and_naloxone();
    test_overdose_kills();
    test_alcohol_kinetics();
    test_interaction();
    test_alcohol_cold();
    test_aspiration();
    test_painkiller_with_drink();
    test_spirits_disinfect();
    test_drug_replay();
    test_drug_feelings();
}
