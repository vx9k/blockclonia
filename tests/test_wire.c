/* Multiplayer byte encoding (wire.c) and the structs sent with it: the
 * reader refuses overruns, non-finite floats, out-of-range fields and
 * control characters; a battered body and a moving player survive the
 * trip exactly, and a damaged one leaves the destination alone. */
#include "health.h"
#include "mem.h"
#include "physics.h"
#include "test_util.h"
#include "wire.h"

#include <math.h>
#include <string.h>

static void test_reader(void)
{
    uint8_t buf[64] = {0};
    wire_w w = wire_writer(buf, sizeof buf);
    wire_u8(&w, 7);
    wire_u16(&w, 0xBEEF);
    wire_u32(&w, 0xDEADBEEFu);
    wire_u64(&w, 0x0123456789ABCDEFull);
    wire_i32(&w, -5);
    wire_f32(&w, 1.5f);
    wire_f64(&w, -2.25);
    wire_str(&w, "hi there", 32);
    CHECK(!w.bad && w.len == 1 + 2 + 4 + 8 + 4 + 4 + 8 + 9);
    CHECK(buf[1] == 0xEF && buf[2] == 0xBE); /* little-endian */

    wire_r r = wire_reader(buf, w.len);
    char s[16] = {0};
    CHECK(wire_get_u8(&r) == 7 && wire_get_u16(&r) == 0xBEEF && wire_get_u32(&r) == 0xDEADBEEFu);
    CHECK(wire_get_u64(&r) == 0x0123456789ABCDEFull && wire_get_i32(&r) == -5);
    CHECK(wire_get_f32(&r) == 1.5f && wire_get_f64(&r) == -2.25);
    wire_get_str(&r, s, sizeof s);
    CHECK(!r.bad && r.pos == w.len && strcmp(s, "hi there") == 0);
    CHECK(wire_get_u8(&r) == 0 && r.bad); /* past the end */

    /* The writer stops at its capacity. */
    wire_w small = wire_writer(buf, 3);
    wire_u32(&small, 1);
    CHECK(small.bad && small.len == 0);
    wire_str(&small, "x", 8);
    CHECK(small.bad);

    /* Non-finite floats, strings too long for the buffer or holding
     * control characters, and ints outside the range are refused. */
    float inf = INFINITY, nan = NAN;
    w = wire_writer(buf, sizeof buf);
    wire_f32(&w, inf);
    r = wire_reader(buf, w.len);
    CHECK(wire_get_f32(&r) == 0.0f && r.bad);
    w = wire_writer(buf, sizeof buf);
    wire_f64(&w, (double)nan);
    r = wire_reader(buf, w.len);
    wire_get_f64(&r);
    CHECK(r.bad);
    w = wire_writer(buf, sizeof buf);
    wire_str(&w, "hello", 8);
    r = wire_reader(buf, w.len);
    wire_get_str(&r, s, 5); /* room for four */
    CHECK(r.bad && s[0] == '\0');
    w = wire_writer(buf, sizeof buf);
    wire_str(&w, "a\nb", 8);
    r = wire_reader(buf, w.len);
    wire_get_str(&r, s, sizeof s);
    CHECK(r.bad);
    w = wire_writer(buf, sizeof buf);
    wire_str(&w, "caf\xc3\xa9", 8);
    r = wire_reader(buf, w.len);
    wire_get_str(&r, s, sizeof s);
    CHECK(!r.bad && strcmp(s, "caf\xc3\xa9") == 0); /* UTF-8 passes */
    w = wire_writer(buf, sizeof buf);
    wire_i32(&w, 9);
    r = wire_reader(buf, w.len);
    CHECK(wire_get_range(&r, 0, 8) == 0 && r.bad);
    /* A length byte claiming more than is there. */
    buf[0] = 200;
    r = wire_reader(buf, 10);
    wire_get_str(&r, s, sizeof s);
    CHECK(r.bad);
}

static void test_health(void)
{
    /* A body with a bit of everything, run long enough for the traces,
     * the log and the slow processes to move. */
    health *a = new_body(1234);
    health_cut(a, BP_LARM, 0.7f, 1, 0.3f);
    health_break_bone(a, BP_RLEG, 1);
    health_dislocate(a, BP_RARM);
    health_burn(a, BP_CHEST, 1.5);
    health_abrasion(a, BP_LLEG, 0.2f, 0.4f);
    health_env env = calm_env();
    env.air_temp = -8.0;
    live(a, &env, 20.0);
    CHECK(a->wound_count >= 3 && a->log[0][0] != '\0');

    uint8_t *buf = mem_alloc(HEALTH_NET_MAX);
    size_t n = health_encode(a, buf, HEALTH_NET_MAX);
    CHECK(n > 0 && n <= HEALTH_NET_MAX);
    CHECK(health_encode(a, buf, 100) == 0); /* does not fit */
    n = health_encode(a, buf, HEALTH_NET_MAX);

    /* Into a zeroed body: everything but the older trace samples arrives,
     * so outside the traces the two match byte for byte (both were
     * zeroed first, so padding agrees). */
    health *b = mem_calloc(1, sizeof *b);
    CHECK(health_decode(b, buf, n) == 0);
    CHECK(memcmp(a, b, offsetof(health, ecg)) == 0);
    /* The log lines are strings: what follows their NUL is not sent. */
    CHECK(a->wave_pos == b->wave_pos && a->wave_acc == b->wave_acc);
    for (int i = 0; i < HEALTH_LOG; i++) CHECK(strcmp(a->log[i], b->log[i]) == 0 && a->log_age[i] == b->log_age[i]);
    int newest = (a->wave_pos + HEALTH_WAVE_LEN - 1) % HEALTH_WAVE_LEN;
    CHECK(b->ecg[newest] == a->ecg[newest] && b->capno[newest] == a->capno[newest]);
    int older = (a->wave_pos + HEALTH_WAVE_LEN - HEALTH_NET_WAVE - 1) % HEALTH_WAVE_LEN;
    CHECK(b->art[older] == 0.0f);

    /* Every truncation, and a byte too many, is refused without touching
     * the destination. */
    health *c = new_body(99);
    health *keep = mem_alloc(sizeof *keep);
    memcpy(keep, c, sizeof *c);
    int refused = 1;
    for (size_t cut = 0; cut < n; cut += 7) refused &= health_decode(c, buf, cut) == -1;
    CHECK(refused);
    CHECK(health_decode(c, buf, n + 1) == -1);
    CHECK(memcmp(c, keep, sizeof *c) == 0); /* NOLINT: untouched means byte for byte, padding too */
    /* An out-of-range rhythm (offset: the six organs, then wound_count). */
    uint8_t *bad = mem_alloc(HEALTH_NET_MAX);
    memcpy(bad, buf, n);
    size_t rhythm_at = 6 * 4 + 4 + 9 * 4 + 13 * 4;
    CHECK(bad[rhythm_at] == (uint8_t)a->rhythm);
    bad[rhythm_at] = RHYTHM_COUNT;
    CHECK(health_decode(c, bad, n) == -1);
    /* A wound count past the array. */
    memcpy(bad, buf, n);
    bad[24] = HEALTH_MAX_WOUNDS + 1;
    CHECK(health_decode(c, bad, n) == -1);
    CHECK(memcmp(c, keep, sizeof *c) == 0); /* NOLINT: untouched means byte for byte, padding too */
    mem_free(bad);
    mem_free(keep);
    mem_free(c);
    mem_free(b);
    mem_free(buf);
    mem_free(a);
}

static void test_player(void)
{
    player p = {0};
    p.pos = dv3(12.5, 70.25, -3.125);
    p.prev_pos = dv3(12.4, 70.3, -3.0);
    p.vel = dv3(1.0, -9.5, 0.25);
    p.yaw = 1.25f;
    p.pitch = -0.5f;
    p.on_ground = 1;
    p.submerged = 0.4;
    p.sprinting = 1;
    p.fall_speed = 3.5;
    uint8_t buf[PLAYER_NET_BYTES] = {0};
    size_t n = player_encode(&p, buf, sizeof buf);
    CHECK(n > 0);
    player q = {0};
    uint8_t again[PLAYER_NET_BYTES] = {0};
    CHECK(player_decode(&q, buf, n) == 0 && player_encode(&q, again, sizeof again) == n && memcmp(buf, again, n) == 0);
    CHECK(q.pos.y == p.pos.y && q.vel.y == p.vel.y && q.yaw == p.yaw && q.sprinting == 1 && q.fall_speed == 3.5);
    CHECK(player_decode(&q, buf, n - 1) == -1);

    /* Positions that would overflow the chunk arithmetic, or angles and
     * speeds no physics step produces, are refused. */
    player far = p;
    far.pos.x = 1e12;
    n = player_encode(&far, buf, sizeof buf);
    CHECK(player_decode(&q, buf, n) == -1 && q.pos.x == p.pos.x);
    far = p;
    far.vel.y = -5000.0;
    n = player_encode(&far, buf, sizeof buf);
    CHECK(player_decode(&q, buf, n) == -1);
    far = p;
    far.submerged = 2.0;
    n = player_encode(&far, buf, sizeof buf);
    CHECK(player_decode(&q, buf, n) == -1);
}

void test_wire_all(void)
{
    test_reader();
    test_health();
    test_player();
}
