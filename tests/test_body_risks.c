/* Treatments that harm when they are the wrong thing, and the adrenaline
 * the body makes itself: an injector, a manual shock, a painkiller
 * overdose, antibiotics with nothing to treat, and the rush after a sudden
 * injury and the crash after it. */
#include "health.h"
#include "mem.h"
#include "test_util.h"

#include <string.h>

void test_body_risks_all(void)
{
    health_env e = calm_env();
    char msg[128] = {0};
    inventory inv = {0};
    inv_init(&inv);

    /* Adrenaline on a healthy heart: the rate climbs and the hands shake. */
    health *h = new_body(21);
    live(h, &e, 10.0);
    float hr0 = h->hr;
    CHECK(!health_treat(h, &inv, BP_CHEST, TREAT_ADRENALINE, 0, msg, sizeof msg)); /* none carried */
    inv_add(&inv, I_ADRENALINE, 2);
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_ADRENALINE, 0, msg, sizeof msg) && inv_count(&inv, I_ADRENALINE) == 1);
    live(h, &e, 30.0);
    CHECK(h->hr > hr0 + 10.0f && health_get_senses(h).tremor > 0.3f);
    mem_free(h);

    /* The override shocks only with the pads on, and does so on a beating
     * heart (burns and pain, whatever the rhythm does next). */
    h = new_body(22);
    CHECK(!health_treat(h, &inv, BP_CHEST, TREAT_SHOCK, 0, msg, sizeof msg));
    health_pads(h, 1);
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_SHOCK, 0, msg, sizeof msg) && h->defib.shocks == 1);
    CHECK(h->part[BP_CHEST].burn_area > 0.0f);
    mem_free(h);

    /* Painkillers: four a day are fine; the sixth starts liver damage that
     * shows over the following game hours, not at once. */
    h = new_body(23);
    inv_add(&inv, I_PAINKILLER, 6);
    for (int i = 0; i < 4; i++) health_treat(h, &inv, BP_CHEST, TREAT_PAINKILLER, 0, msg, sizeof msg);
    CHECK(h->hepatotox == 0.0f);
    for (int i = 0; i < 2; i++) health_treat(h, &inv, BP_CHEST, TREAT_PAINKILLER, 0, msg, sizeof msg);
    float liver0 = h->organ[ORG_LIVER];
    CHECK(h->hepatotox > 0.1f && liver0 > 0.99f);
    live(h, &e, 600.0); /* 12 game hours */
    CHECK(h->organ[ORG_LIVER] < liver0 - 0.1f);
    mem_free(h);

    /* Antibiotics with no infection: an upset gut and water lost. */
    h = new_body(24);
    inv_add(&inv, I_ANTIBIOTIC, 1);
    CHECK(health_treat(h, &inv, BP_CHEST, TREAT_ANTIBIOTIC, 0, msg, sizeof msg) && h->gut_upset > 0.0f);
    CHECK(h->organ[ORG_GUT] < 1.0f);
    mem_free(h);

    /* A sudden bad injury sets off a rush that dulls the pain, and a crash
     * follows once it wears off. */
    h = new_body(25);
    live(h, &e, 5.0);
    health_break_bone(h, BP_LLEG, 0);
    live(h, &e, 2.0);
    CHECK(h->surge > 0.0f || h->catechol > 0.1f);
    live(h, &e, 60.0);
    CHECK(h->crash > 0.0f);
    mem_free(h);
}
