/* Saving the whole body (health_tags.c, the player record in survival.c)
 * and the world's state (world_state.c): a body comes back byte for byte
 * and steps on exactly as if it had never left, a dead one stays dead,
 * damaged records are refused, and records from before a field existed
 * load with that field at its default. Leaving a world or a crash must
 * neither heal nor harm. */
#include "gamemode.h"
#include "health.h"
#include "inventory.h"
#include "mem.h"
#include "save_batch.h"
#include "save_db.h"
#include "survival.h"
#include "tagged.h"
#include "test_util.h"
#include "thermo.h"
#include "world_state.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Bodies compare byte for byte, padding included: every one here starts
 * from health_init's memset and is copied whole, so padding is zero in
 * both and only a field the record lost can differ. */
static int same(const void *a, const void *b, size_t n) { return memcmp(a, b, n) == 0; }

/* A battered body: every debug injury the menu offers on a few parts,
 * lived through for a while so every system is mid-flight. */
static health *battered(uint32_t seed, int first, int count, double seconds)
{
    health *h = mem_alloc(sizeof *h);
    health_init_varied(h, seed);
    health_settle(h);
    for (int k = first; k < first + count && k < HEALTH_DEBUG_COUNT; k++) health_injure(h, k, k % BP_COUNT);
    health_env e = calm_env();
    live(h, &e, seconds);
    return h;
}

/* The body through a player record, into a fresh body of another seed. */
static int trip(const health *h, health *out)
{
    uint8_t *buf = mem_alloc(PLAYER_RECORD_MAX);
    player p = {0}, q = {0};
    p.pos = dv3(0.5, GROUND, 0.5);
    inventory inv = {0}, qi = {0};
    inv_starting_kit(&inv);
    size_t n = survival_encode_player(&p, &inv, h, buf, PLAYER_RECORD_MAX);
    health_init_varied(out, 99);
    int r = n ? survival_decode_player(&q, &qi, out, buf, n) : -1;
    mem_free(buf);
    return r;
}

static void test_body_round_trip(void)
{
    health *back = mem_alloc(sizeof *back);
    /* Every injury, a few at a time, so fractures, burns, drugs, the
     * heart's rhythms and the defibrillator all go through. */
    for (int first = 0; first < HEALTH_DEBUG_COUNT; first += 4) {
        health *h = battered(11u + (uint32_t)first, first, 4, 6.0);
        CHECK(trip(h, back) == 0);
        CHECK(same(h, back, sizeof *h));
        mem_free(h);
    }
    mem_free(back);
}

/* The real promise: after a reload the body goes on exactly as the one
 * that stayed would have, bleeding, healing and beating alike. */
static void test_body_steps_on(void)
{
    health *h = battered(5, 0, 3, 4.0);
    health *back = mem_alloc(sizeof *back);
    CHECK(trip(h, back) == 0);
    health_env e = calm_env();
    live(h, &e, 30.0);
    live(back, &e, 30.0);
    CHECK(same(h, back, sizeof *h));
    CHECK(h->blood == back->blood && h->hr == back->hr);
    mem_free(back);
    mem_free(h);
}

static void test_body_dead_stays_dead(void)
{
    health *h = battered(6, 0, 1, 1.0);
    h->dead = 1;
    h->cause = DEATH_BLOOD_LOSS;
    h->blood = 1.2f;
    health *back = mem_alloc(sizeof *back);
    CHECK(trip(h, back) == 0);
    CHECK(back->dead == 1 && back->cause == DEATH_BLOOD_LOSS && back->blood == 1.2f);
    mem_free(back);
    mem_free(h);
}

/* Out-of-range indexes and enums are refused and the destination left as
 * it was, so a crafted world cannot index past an array or hang a loop. */
static void test_body_refuses_damage(void)
{
    health *h = battered(8, 0, 2, 1.0);
    health *back = mem_alloc(sizeof *back), *was = mem_alloc(sizeof *was);
    for (int c = 0; c < 6; c++) {
        health *bad = mem_alloc(sizeof *bad);
        memcpy(bad, h, sizeof *bad);
        switch (c) {
        case 0: bad->wound_count = HEALTH_MAX_WOUNDS + 1; break;
        case 1: bad->rhythm = RHYTHM_COUNT; break;
        case 2: bad->wave_acc = 1e9f; break;
        case 3: bad->rng = 0; break;
        case 4: bad->ans.hist_pos = AUTO_HIST; break;
        default: bad->wounds[0].part = BP_COUNT; break;
        }
        health_init_varied(back, 99);
        memcpy(was, back, sizeof *was);
        uint8_t *buf = mem_alloc(PLAYER_RECORD_MAX);
        player p = {0}, q = {0};
        p.pos = dv3(0.5, GROUND, 0.5);
        inventory inv = {0};
        inv_init(&inv);
        size_t n = survival_encode_player(&p, &inv, bad, buf, PLAYER_RECORD_MAX);
        CHECK(n > 0 && survival_decode_player(&q, &inv, back, buf, n) == -1);
        CHECK(same(back, was, sizeof *back));
        mem_free(buf);
        mem_free(bad);
    }
    mem_free(was);
    mem_free(back);
    mem_free(h);
}

/* A record without a body (a build from before bodies were saved, or one
 * that leaves it out) keeps the fresh body; text is made safe to print. */
static void test_body_missing_and_text(void)
{
    uint8_t buf[4096] = {0};
    player p = {0}, q = {0};
    p.pos = dv3(0.5, GROUND, 0.5);
    inventory inv = {0};
    inv_init(&inv);
    size_t n = survival_encode_player(&p, &inv, NULL, buf, sizeof buf);
    health *fresh = mem_alloc(sizeof *fresh), *was = mem_alloc(sizeof *was);
    health_init_varied(fresh, 3);
    memcpy(was, fresh, sizeof *was);
    CHECK(survival_decode_player(&q, &inv, fresh, buf, n) == 0 && same(fresh, was, sizeof *was));

    health *h = battered(4, 0, 1, 0.5);
    memcpy(h->log[0],
           "\x01"
           "bad\x7f",
           6);
    memset(h->log[1], 'x', sizeof h->log[1]); /* no terminator */
    CHECK(trip(h, fresh) == 0);
    CHECK(strcmp(fresh->log[0], "?bad?") == 0 && strlen(fresh->log[1]) == sizeof h->log[1] - 1);
    mem_free(h);
    mem_free(was);
    mem_free(fresh);

    /* No two fields of a shipped table share a key. */
    CHECK(tag_keys_distinct(&g_health_tags) && tag_keys_distinct(&g_inv_tags));
}

static void test_world_state(void)
{
    thermo *a = thermo_create(1), *b = thermo_create(1);
    thermo_set_day_time(a, 0.3);
    thermo_set_days(a, 5);
    uint8_t buf[WORLD_STATE_MAX] = {0};
    size_t n = world_state_encode(a, buf, sizeof buf);
    CHECK(n > 4 && memcmp(buf, "MCWS", 4) == 0);
    CHECK(world_state_decode(b, buf, n) == 0 && thermo_day_time(b) == 0.3 && thermo_days(b) == 5);
    /* The planned systems already have their (empty) sections. */
    wire_r r = wire_reader(buf + 4, n - 4);
    CHECK(wire_get_u16(&r) == 5);

    /* Cut short, or under another magic, it is refused and b kept. */
    thermo_set_day_time(b, 0.75);
    for (size_t cut = 0; cut < n; cut++) CHECK(world_state_decode(b, buf, cut) == -1);
    CHECK(thermo_day_time(b) == 0.75 && thermo_days(b) == 5);

    /* A record without a clock (an older build's) leaves the clock alone. */
    uint8_t none[6] = {'M', 'C', 'W', 'S', 0, 0};
    CHECK(world_state_decode(b, none, sizeof none) == 0 && thermo_day_time(b) == 0.75);

    /* Midnight counts a day. */
    test_world t = {0};
    tw_init(&t);
    thermo_set_day_time(a, 0.999);
    thermo_step(a, &t.w, 60.0, dv3(0.5, GROUND, 0.5));
    CHECK(thermo_days(a) == 6 && thermo_day_time(a) < 0.1);
    tw_free(&t);
    thermo_destroy(a);
    thermo_destroy(b);
}

/* The exploit reported: quit with a hurt (or dead) body and open the world
 * again. Through the database the body comes back as it was. */
static void test_rejoin_keeps_body(void)
{
    char dir[400] = {0};
    CHECK(tmp_dir_make(dir, sizeof dir, "mc_body_"));
    health *h = battered(9, 0, 2, 3.0);
    health *back = mem_alloc(sizeof *back);
    uint8_t *buf = mem_alloc(PLAYER_RECORD_MAX + 1);
    for (int dead = 0; dead < 2; dead++) {
        h->dead = dead;
        save_db *db = save_db_open(dir);
        CHECK(db != NULL);
        if (!db) break;
        player p = {0};
        p.pos = dv3(0.5, GROUND, 0.5);
        inventory inv = {0};
        inv_starting_kit(&inv);
        size_t n = survival_encode_player(&p, &inv, h, buf, PLAYER_RECORD_MAX);
        CHECK(n && save_db_put_player(db, SAVE_DB_LOCAL_PLAYER, buf, n) == 0);
        save_db_close(db);

        db = save_db_open(dir);
        CHECK(db != NULL);
        if (!db) break;
        long got = save_db_get_player(db, SAVE_DB_LOCAL_PLAYER, buf, PLAYER_RECORD_MAX + 1);
        health_init_varied(back, 1);
        player q = {0};
        CHECK(got > 0 && survival_decode_player(&q, &inv, back, buf, (size_t)got) == 0);
        CHECK(same(back, h, sizeof *h) && back->dead == dead);
        save_db_close(db);
    }
    mem_free(buf);
    mem_free(back);
    mem_free(h);
    const char *files[] = {"world.db", "world.db-wal", "world.db-shm", "world.db-journal"};
    char p[512] = {0};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        tmp_remove(p);
    }
    tmp_remove(dir);
}

/* Every condition the debug menu can cause, one at a time, goes on after a
 * reload exactly as it would have: arrests, drugs, infections, burns. */
static void test_body_steps_on_every_kind(void)
{
    health *back = mem_alloc(sizeof *back);
    health_env e = calm_env();
    for (int k = 0; k < HEALTH_DEBUG_COUNT; k++) {
        health *h = battered(100u + (uint32_t)k, k, 1, 2.0);
        CHECK(trip(h, back) == 0);
        live(h, &e, 5.0);
        live(back, &e, 5.0);
        if (!same(h, back, sizeof *h)) printf("  %s diverges after a reload\n", HEALTH_DEBUG_KINDS[k].name);
        CHECK(same(h, back, sizeof *h));
        mem_free(h);
    }
    mem_free(back);
}

/* Sections as a record of one: the count, then each section's bytes. */
static size_t one_section(const char *name, const void *src, const tag_table *t, uint8_t *out, size_t cap)
{
    tag_section sec[] = {{name, src, NULL, t}};
    wire_w w = wire_writer(out, cap);
    tag_put_sections(&w, sec, 1);
    return w.bad ? 0 : w.len;
}

typedef struct {
    float seat[3];
    int32_t gear;
} future_seat;
static const tag_field SEAT_F[] = {TAG_F32(future_seat, seat), TAG_I32(future_seat, gear)};
static const tag_table SEAT = TAG_TABLE(SEAT_F);

/* A record from a later build, with a section this build has never heard
 * of, loads what this build knows; trailing bytes and cuts do not. */
static void test_player_forward(void)
{
    uint8_t *buf = mem_alloc(PLAYER_RECORD_MAX), extra[256] = {0};
    health *h = battered(21, 0, 1, 1.0), *back = mem_alloc(sizeof *back);
    player p = {0}, q = {0};
    p.pos = dv3(4.5, GROUND, -7.5);
    p.yaw = 0.5f;
    inventory inv = {0}, qi = {0};
    inv_starting_kit(&inv);
    size_t n = survival_encode_player(&p, &inv, h, buf, PLAYER_RECORD_MAX);
    future_seat seat = {{1.0f, 2.0f, 3.0f}, 4};
    size_t xn = one_section("vehicle_seat", &seat, &SEAT, extra, sizeof extra);
    CHECK(n && xn > 2 && n + xn - 2 < PLAYER_RECORD_MAX);
    /* Three sections become four. */
    CHECK(buf[4] == 3 && buf[5] == 0);
    buf[4] = 4;
    memcpy(buf + n, extra + 2, xn - 2);
    size_t later = n + xn - 2;
    health_init_varied(back, 2);
    CHECK(survival_decode_player(&q, &qi, back, buf, later) == 0);
    CHECK(q.pos.x == 4.5 && q.yaw == 0.5f && same(&qi, &inv, sizeof inv) && same(back, h, sizeof *h));

    /* A caller that wants no body leaves the body's section unread. */
    q = (player){0};
    CHECK(survival_decode_player(&q, &qi, NULL, buf, later) == 0 && q.pos.z == -7.5);

    /* One byte too many, or any cut, and nothing is taken. */
    buf[later] = 0;
    q.yaw = 2.0f;
    CHECK(survival_decode_player(&q, &qi, NULL, buf, later + 1) == -1 && q.yaw == 2.0f);
    for (size_t cut = 0; cut < later; cut += 97) CHECK(survival_decode_player(&q, &qi, NULL, buf, cut) == -1);
    CHECK(q.yaw == 2.0f);
    mem_free(back);
    mem_free(h);
    mem_free(buf);
}

/* What the player section may not hold, each refused on its own. */
static void test_player_limits(void)
{
    uint8_t buf[4096] = {0};
    inventory inv = {0}, qi = {0};
    inv_init(&inv);
    player good = {0};
    good.pos = good.prev_pos = dv3(0.5, GROUND, 0.5);
    for (int c = 0; c < 8; c++) {
        player p = good;
        switch (c) {
        case 0: p.vel.x = 1001.0; break;
        case 1: p.vel.y = -1e9; break;
        case 2: p.prev_pos.z = 1e12; break;
        case 3: p.pos.y = -65.0; break;
        case 4: p.pitch = 2.0f; break;
        case 5: p.mode = MODE_COUNT; break;
        case 6: p.height = 4.0f; break;
        default: p.yaw = -INFINITY; break;
        }
        size_t n = survival_encode_player(&p, &inv, NULL, buf, sizeof buf);
        player q = {0};
        q.yaw = 7.0f;
        CHECK(n && survival_decode_player(&q, &qi, NULL, buf, n) == -1 && q.yaw == 7.0f);
    }
    /* The edges themselves are fine. */
    player edge = good;
    edge.vel = dv3(1000.0, -1000.0, 0.0);
    edge.pos.y = -64.0;
    size_t n = survival_encode_player(&edge, &inv, NULL, buf, sizeof buf);
    player q = {0};
    CHECK(n && survival_decode_player(&q, &qi, NULL, buf, n) == 0 && q.pos.y == -64.0);
}

/* Every way an inventory can be impossible. */
static void test_inv_valid(void)
{
    inventory ok = {0};
    inv_starting_kit(&ok);
    CHECK(inv_valid(&ok));
    /* Empty is valid, and so is a stack held on the cursor. */
    inventory empty = {0};
    inv_init(&empty);
    CHECK(inv_valid(&empty));
    empty.cursor = (item_stack){I_APPLE, 3, 0};
    CHECK(inv_valid(&empty));
    for (int c = 0; c < 7; c++) {
        inventory v = ok;
        switch (c) {
        case 0: v.selected = -1; break;
        case 1: v.selected = INV_HOTBAR; break;
        case 2: v.cursor = (item_stack){0, 3, 0}; break;
        case 3: v.slot[5] = (item_stack){I_APPLE, 0, 0}; break;
        case 4: v.slot[5] = (item_stack){0xFFFFFFu, 1, 0}; break;
        case 5: v.slot[INV_SLOTS - 1] = (item_stack){I_APPLE, 1, 1}; break; /* apples do not wear */
        default: v.slot[0] = (item_stack){B_DIRT, 200, 0}; break;
        }
        CHECK(!inv_valid(&v));
    }
}

/* The clock section as a later or a broken build might write it. */
typedef struct {
    double day;
    int64_t days;
    int32_t moon;
} odd_clock;
static const tag_field ODD_F[] = {TAG_F64(odd_clock, day), TAG_I64(odd_clock, days), TAG_I32(odd_clock, moon)};
static const tag_table ODD = TAG_TABLE(ODD_F);

static size_t clock_record(odd_clock c, int with_tides, uint8_t *out, size_t cap)
{
    future_seat tides = {{0.5f, 0.0f, 0.0f}, 2};
    tag_section sec[] = {{"clock", &c, NULL, &ODD}, {"tides", &tides, NULL, &SEAT}};
    wire_w w = wire_writer(out, cap);
    wire_bytes(&w, (const uint8_t *)"MCWS", 4);
    tag_put_sections(&w, sec, with_tides ? 2 : 1);
    return w.bad ? 0 : w.len;
}

static void test_world_state_fields(void)
{
    thermo *th = thermo_create(2);
    uint8_t buf[WORLD_STATE_MAX] = {0};
    /* A later build's clock, with a field and a section this one lacks. */
    size_t n = clock_record((odd_clock){0.25, 12, 3}, 1, buf, sizeof buf);
    CHECK(n && world_state_decode(th, buf, n) == 0 && thermo_day_time(th) == 0.25 && thermo_days(th) == 12);
    /* A time of day outside a day, or days before the first, is refused. */
    odd_clock bad[] = {{1.5, 0, 0}, {-0.1, 0, 0}, {0.5, -1, 0}, {0.5, (int64_t)2e15, 0}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        n = clock_record(bad[i], 0, buf, sizeof buf);
        CHECK(n && world_state_decode(th, buf, n) == -1);
    }
    CHECK(thermo_day_time(th) == 0.25 && thermo_days(th) == 12);
    /* Days never count below zero. */
    thermo_set_days(th, -5);
    CHECK(thermo_days(th) == 0);
    CHECK(world_state_decode(th, NULL, 0) == -1);
    thermo_destroy(th);
}

/* Through the database: none yet, then saved, then damaged. */
static void test_world_state_db(void)
{
    char dir[400] = {0};
    CHECK(tmp_dir_make(dir, sizeof dir, "mc_state_"));
    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (db) {
        thermo *a = thermo_create(1), *b = thermo_create(1);
        thermo_set_day_time(b, 0.6);
        CHECK(world_state_load(db, b) == 0 && thermo_day_time(b) == 0.6);
        thermo_set_day_time(a, 0.125);
        thermo_set_days(a, 40);
        save_batch sb = {0};
        CHECK(world_state_save(&sb, a) == 0 && sb.n == 1);
        save_db_submit(db, &sb);
        CHECK(world_state_load(db, b) == 1 && thermo_day_time(b) == 0.125 && thermo_days(b) == 40);
        CHECK(save_db_put_blob(db, "state", "MCWSjunk", 8) == 0);
        thermo_set_days(b, 7);
        CHECK(world_state_load(db, b) == -1 && thermo_days(b) == 7);
        thermo_destroy(a);
        thermo_destroy(b);
        save_db_close(db);
    }
    const char *files[] = {"world.db", "world.db-wal", "world.db-shm", "world.db-journal"};
    char p[512] = {0};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        tmp_remove(p);
    }
    tmp_remove(dir);
}

void test_save_body_all(void)
{
    TEST_PART(test_body_round_trip);
    TEST_PART(test_body_steps_on);
    TEST_PART(test_body_dead_stays_dead);
    TEST_PART(test_body_refuses_damage);
    TEST_PART(test_body_missing_and_text);
    TEST_PART(test_world_state);
    TEST_PART(test_rejoin_keeps_body);
    TEST_PART(test_body_steps_on_every_kind);
    TEST_PART(test_player_forward);
    TEST_PART(test_player_limits);
    TEST_PART(test_inv_valid);
    TEST_PART(test_world_state_fields);
    TEST_PART(test_world_state_db);
}
