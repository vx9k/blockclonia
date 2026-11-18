/* Game modes (gamemode.c): what each mode permits, the world's rules on
 * disk and their checks, switching, hardcore's one life, and how physics,
 * survival and the hands follow the player's mode. */
#include "fx.h"
#include "gamemode.h"
#include "interact.h"
#include "inventory.h"
#include "mem.h"
#include "survival.h"
#include "test_util.h"
#include "worlds.h"

#include <stdio.h>
#include <string.h>

static void test_mode_table(void)
{
    CHECK(strcmp(mode_name(MODE_SURVIVAL), "Survival") == 0);
    CHECK(strcmp(mode_name(MODE_SPECTATOR), "Spectator") == 0);
    CHECK(strcmp(mode_name(-1), "?") == 0 && strcmp(mode_name(MODE_COUNT), "?") == 0);
    CHECK(mode_valid(MODE_HARDCORE) && !mode_valid(-1) && !mode_valid(MODE_COUNT));
    /* The body lives only where death means something. */
    CHECK(mode_body(MODE_SURVIVAL) && mode_body(MODE_HARDCORE));
    CHECK(!mode_body(MODE_CREATIVE) && !mode_body(MODE_SPECTATOR));
    CHECK(mode_creative(MODE_CREATIVE) && !mode_creative(MODE_SURVIVAL) && !mode_creative(MODE_SPECTATOR));
    CHECK(!mode_can_fly(MODE_SURVIVAL) && !mode_can_fly(MODE_HARDCORE));
    CHECK(mode_can_fly(MODE_CREATIVE) && mode_can_fly(MODE_SPECTATOR));
    CHECK(mode_hands(MODE_CREATIVE) && !mode_hands(MODE_SPECTATOR));
    CHECK(mode_noclip(MODE_SPECTATOR) && !mode_noclip(MODE_CREATIVE));
}

static void test_rules_new_and_switch(void)
{
    world_rules r = rules_new(MODE_CREATIVE, 1);
    CHECK(r.mode == MODE_CREATIVE && r.cheats == 1 && r.hardcore == 0);
    r = rules_new(MODE_HARDCORE, 1);
    CHECK(r.mode == MODE_HARDCORE && r.cheats == 0 && r.hardcore == 1);
    r = rules_new(99, 7);
    CHECK(r.mode == MODE_SURVIVAL && r.cheats == 1);

    /* With cheats: survival -> creative -> spectator -> survival, never
     * into hardcore. */
    world_rules c = rules_new(MODE_SURVIVAL, 1);
    CHECK(rules_next_mode(&c, MODE_SURVIVAL) == MODE_CREATIVE);
    CHECK(rules_next_mode(&c, MODE_CREATIVE) == MODE_SPECTATOR);
    CHECK(rules_next_mode(&c, MODE_SPECTATOR) == MODE_SURVIVAL);
    for (int m = 0; m < MODE_COUNT; m++) CHECK(rules_next_mode(&c, m) != MODE_HARDCORE);
    /* Without cheats, and in hardcore, the mode stays. */
    world_rules plain = rules_new(MODE_SURVIVAL, 0);
    CHECK(rules_next_mode(&plain, MODE_SURVIVAL) == MODE_SURVIVAL);
    world_rules hc = rules_new(MODE_HARDCORE, 1);
    CHECK(rules_next_mode(&hc, MODE_HARDCORE) == MODE_HARDCORE);
    CHECK(rules_next_mode(NULL, MODE_CREATIVE) == MODE_CREATIVE);

    CHECK(gamemode_after_death(MODE_HARDCORE) == MODE_SPECTATOR);
    CHECK(gamemode_after_death(MODE_SURVIVAL) == MODE_SURVIVAL);
    CHECK(gamemode_after_death(MODE_CREATIVE) == MODE_CREATIVE);
}

static void test_apply_and_fly(void)
{
    player p = {0};
    p.flying = 1;
    p.sprinting = 1;
    gamemode_apply(&p, MODE_SURVIVAL);
    CHECK(p.mode == MODE_SURVIVAL && !p.flying && !p.sprinting);
    /* F does nothing in survival, the loophole this closes. */
    CHECK(gamemode_toggle_fly(&p) == 0 && !p.flying);

    gamemode_apply(&p, MODE_CREATIVE);
    CHECK(!p.flying);
    p.vel = dv3(3, 4, 5);
    CHECK(gamemode_toggle_fly(&p) == 1 && p.vel.x == 0.0 && p.vel.y == 0.0);
    CHECK(gamemode_toggle_fly(&p) == 0);

    /* A spectator always flies, and the key cannot ground it. */
    gamemode_apply(&p, MODE_SPECTATOR);
    CHECK(p.flying);
    CHECK(gamemode_toggle_fly(&p) == 1 && p.flying);
    gamemode_apply(&p, 42);
    CHECK(p.mode == MODE_SURVIVAL && !p.flying);
}

/* The rules in world.db: written with the world, read back, and checked
 * like everything else read from a shared file. */
static void test_rules_on_disk(void)
{
    char root[256] = {0}, dir[512] = {0};
    if (!tmp_dir_make(root, sizeof root, "mc_mode")) {
        CHECK(0);
        return;
    }
    world_rules want = rules_new(MODE_CREATIVE, 1);
    CHECK(worlds_create(root, "Modes", 7, &want, dir, sizeof dir) == 0);
    save_peek pk = {0};
    save_db_peek(dir, &pk);
    CHECK(pk.status == SAVE_PEEK_OK && pk.mode == MODE_CREATIVE && pk.hardcore == 0);

    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (db) {
        world_rules r = rules_load(db);
        CHECK(r.mode == MODE_CREATIVE && r.cheats == 1 && r.hardcore == 0);

        /* Out of range, or not 0/1: survival without cheats. */
        CHECK(save_db_set_meta(db, "mode", 9) == 0);
        r = rules_load(db);
        CHECK(r.mode == MODE_SURVIVAL && r.cheats == 0);
        CHECK(save_db_set_meta(db, "mode", MODE_CREATIVE) == 0);
        CHECK(save_db_set_meta(db, "cheats", 2) == 0);
        r = rules_load(db);
        CHECK(r.mode == MODE_SURVIVAL && r.cheats == 0);
        CHECK(save_db_set_meta(db, "cheats", 1) == 0);
        CHECK(save_db_set_meta(db, "hardcore", -1) == 0);
        CHECK(rules_load(db).mode == MODE_SURVIVAL);

        /* A hardcore world edited to allow cheats or creative keeps its
         * single life; a hardcore mode without the flag is survival. */
        CHECK(save_db_set_meta(db, "hardcore", 1) == 0);
        r = rules_load(db);
        CHECK(r.hardcore == 1 && r.cheats == 0 && r.mode == MODE_HARDCORE);
        CHECK(save_db_set_meta(db, "mode", MODE_SPECTATOR) == 0);
        CHECK(rules_load(db).mode == MODE_SPECTATOR);
        CHECK(save_db_set_meta(db, "hardcore", 0) == 0);
        CHECK(save_db_set_meta(db, "mode", MODE_HARDCORE) == 0);
        CHECK(rules_load(db).mode == MODE_SURVIVAL);

        world_rules hc = rules_new(MODE_HARDCORE, 0);
        hc.mode = gamemode_after_death(hc.mode);
        CHECK(rules_save(db, &hc) == 0);
        r = rules_load(db);
        CHECK(r.hardcore == 1 && r.mode == MODE_SPECTATOR);
        CHECK(rules_save(NULL, &hc) == -1 && rules_save(db, NULL) == -1);
        save_db_close(db);
    }
    save_db_peek(dir, &pk);
    CHECK(pk.mode == MODE_SPECTATOR && pk.hardcore == 1);

    /* A world made without rules, or a missing database: survival. */
    char plain[512] = {0};
    CHECK(worlds_create(root, "Plain", 8, NULL, plain, sizeof plain) == 0);
    save_db_peek(plain, &pk);
    CHECK(pk.mode == MODE_SURVIVAL && pk.hardcore == 0);
    world_rules none = rules_load(NULL);
    CHECK(none.mode == MODE_SURVIVAL && none.cheats == 0 && none.hardcore == 0);
    CHECK(worlds_delete(dir) == 0 && worlds_delete(plain) == 0);
    tmp_remove(root);
}

/* The mode crosses the network with the player, and a bad one is refused. */
static void test_mode_on_wire(void)
{
    player p = {0};
    p.pos = p.prev_pos = dv3(1.5, 40.0, 2.5);
    gamemode_apply(&p, MODE_SPECTATOR);
    uint8_t buf[PLAYER_NET_BYTES] = {0};
    size_t n = player_encode(&p, buf, sizeof buf);
    CHECK(n > 0);
    player q = {0};
    CHECK(player_decode(&q, buf, n) == 0 && q.mode == MODE_SPECTATOR && q.flying);
    p.mode = 7;
    n = player_encode(&p, buf, sizeof buf);
    q.mode = MODE_CREATIVE;
    CHECK(player_decode(&q, buf, n) == -1 && q.mode == MODE_CREATIVE);
    p.mode = -1;
    n = player_encode(&p, buf, sizeof buf);
    CHECK(player_decode(&q, buf, n) == -1);
}

/* How much a fall has done to the body: lost tissue, broken bones,
 * sprains, pain and open wounds. */
static double harm(const health *h)
{
    double sum = h->wound_count;
    for (int i = 0; i < BP_COUNT; i++) {
        const body_part *p = &h->part[i];
        sum += (double)(1.0f - p->integrity) + p->fracture + (double)p->sprain + (double)p->pain;
    }
    return sum;
}

static void test_modes_in_play(void)
{
    test_world t = {0};
    tw_init(&t);
    player p = {0};
    player_spawn(&p, &t.w, 0.5, 0.5);
    CHECK(p.mode == MODE_SURVIVAL);
    /* A respawn keeps the mode. */
    gamemode_apply(&p, MODE_SPECTATOR);
    player_spawn(&p, &t.w, 0.5, 0.5);
    CHECK(p.mode == MODE_SPECTATOR && p.flying && p.pos.y == GROUND);
    /* Rubbish in the struct comes out as survival. */
    p.mode = 1234;
    player_spawn(&p, &t.w, 0.5, 0.5);
    CHECK(p.mode == MODE_SURVIVAL);

    /* A spectator sinks through the ground; creative flight stops on it. */
    player_input down = {.descend = 1};
    gamemode_apply(&p, MODE_SPECTATOR);
    run_steps(&t, &p, &down, 60);
    CHECK(p.pos.y < GROUND - 5.0);
    run_steps(&t, &p, &down, 600);
    CHECK(p.pos.y >= -32.0); /* but never out of the world */
    player_spawn(&p, &t.w, 0.5, 0.5);
    gamemode_apply(&p, MODE_CREATIVE);
    gamemode_toggle_fly(&p);
    run_steps(&t, &p, &down, 60);
    CHECK(p.pos.y >= GROUND - 1e-6);

    /* Creative and spectator carry no body: nothing hurts or limits it. */
    survival_before b = {0};
    health_env e = {0};
    gamemode_apply(&p, MODE_CREATIVE);
    p.flying = 0;
    survival_env(&t.w, &p, &b, 0, &e);
    CHECK(e.invulnerable);
    gamemode_apply(&p, MODE_SURVIVAL);
    survival_env(&t.w, &p, &b, 0, &e);
    CHECK(!e.invulnerable);
    health *h = new_body(3);
    player_contact contact = {0};
    gamemode_apply(&p, MODE_CREATIVE);
    p.flying = 0;
    b.on_ground = 0;
    b.vel = dv3(0, -30.0, 0);
    p.on_ground = 1;
    double fresh = harm(h);
    survival_impacts(h, &t.w, &contact, &p, &b);
    CHECK(harm(h) == fresh);
    gamemode_apply(&p, MODE_SURVIVAL);
    survival_impacts(h, &t.w, &contact, &p, &b);
    CHECK(harm(h) > fresh); /* a 30 m/s landing hurts */
    mem_free(h);

    /* Hands: creative places for free and breaks at once with no drop;
     * survival pays for the block. (A spectator's hands are kept off by
     * can_act, in main.c and mp.c.) */
    player_spawn(&p, &t.w, 0.5, 0.5);
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, B_PLANKS, 2);
    interact s = {0};
    interact_init(&s, 4);
    fx_state *fx = mem_alloc(sizeof *fx);
    fx_init(fx, 4);
    interact_input use = {.use_click = 1, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    ray_hit hit = {1, {8, GROUND - 1, 8}, {8, GROUND, 8}, B_GRASS};
    dvec3 eye = dv3(8.5, GROUND + 3.0, 8.5);
    gamemode_apply(&p, MODE_CREATIVE);
    interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, v3(0, -1, 0), hit, &use);
    CHECK(world_get(&t.w, 8, GROUND, 8) == B_PLANKS && inv_count(&inv, B_PLANKS) >= 2);
    gamemode_apply(&p, MODE_SURVIVAL);
    inv_set_held(&inv, B_PLANKS, 2);
    hit = (ray_hit){1, {8, GROUND, 8}, {8, GROUND + 1, 8}, B_PLANKS};
    interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, v3(0, -1, 0), hit, &use);
    CHECK(world_get(&t.w, 8, GROUND + 1, 8) == B_PLANKS && inv_held(&inv)->count == 1);

    interact_input hitit = {.attack = 1, .attack_click = 1, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    hit = (ray_hit){1, {8, GROUND + 1, 8}, {8, GROUND + 2, 8}, B_PLANKS};
    gamemode_apply(&p, MODE_CREATIVE);
    int items = t.ph.item_count;
    interact_out o = interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, v3(0, -1, 0), hit, &hitit);
    CHECK(world_get(&t.w, 8, GROUND + 1, 8) == B_AIR && !o.broke && t.ph.item_count == items);
    mem_free(fx);
    tw_free(&t);
}

void test_gamemode_all(void)
{
    test_mode_table();
    test_rules_new_and_switch();
    test_apply_and_fly();
    test_rules_on_disk();
    test_mode_on_wire();
    test_modes_in_play();
}
