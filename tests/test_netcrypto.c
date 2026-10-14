/* Multiplayer encryption (netcrypto.c): both sides derive the same keys
 * whoever dials, a stranger with another identity cannot read or forge,
 * and opening refuses damaged, moved, replayed and too-old packets
 * without moving its replay window. */
#include "netcrypto.h"
#include "test_util.h"

#include <string.h>

typedef struct {
    nc_keypair id, eph;
    uint8_t hello[NC_HELLO_BYTES];
    nc_session s;
} side;

static int pair(side *a, side *b)
{
    nc_hello_make(&a->id, &a->eph, a->hello);
    nc_hello_make(&b->id, &b->eph, b->hello);
    int ra = nc_session_derive(&a->s, &a->id, &a->eph, a->hello, b->hello);
    int rb = nc_session_derive(&b->s, &b->id, &b->eph, b->hello, a->hello);
    return ra == 0 && rb == 0;
}

static void test_handshake(void)
{
    side a = {0}, b = {0};
    nc_keypair_new(&a.id);
    nc_keypair_new(&b.id);
    CHECK(pair(&a, &b));
    CHECK(memcmp(a.s.tx, b.s.rx, NC_KEY_BYTES) == 0 && memcmp(a.s.rx, b.s.tx, NC_KEY_BYTES) == 0);
    CHECK(memcmp(a.s.tx, a.s.rx, NC_KEY_BYTES) != 0);
    CHECK(memcmp(a.s.peer, b.id.pk, NC_KEY_BYTES) == 0 && memcmp(b.s.peer, a.id.pk, NC_KEY_BYTES) == 0);
    const uint8_t zero[NC_KEY_BYTES] = {0};
    CHECK(memcmp(a.eph.sk, zero, NC_KEY_BYTES) == 0); /* the ephemeral secret is wiped */

    /* A new connection between the same players gets new keys. */
    uint8_t old[NC_KEY_BYTES] = {0};
    memcpy(old, a.s.tx, NC_KEY_BYTES);
    CHECK(pair(&a, &b));
    CHECK(memcmp(old, a.s.tx, NC_KEY_BYTES) != 0);

    /* Someone in the middle with their own identity passes on b's
     * ephemeral key under it: the handshake completes, but a reads with
     * keys only the impostor holds, so b's packets fail to open. */
    side m = {0};
    nc_keypair_new(&m.id);
    nc_hello_make(&a.id, &a.eph, a.hello);
    nc_hello_make(&b.id, &b.eph, b.hello);
    uint8_t forged[NC_HELLO_BYTES] = {0};
    memcpy(forged, b.hello, NC_HELLO_BYTES);
    memcpy(forged + 4, m.id.pk, NC_KEY_BYTES);
    CHECK(nc_session_derive(&a.s, &a.id, &a.eph, a.hello, forged) == 0);
    CHECK(nc_session_derive(&b.s, &b.id, &b.eph, b.hello, a.hello) == 0);
    CHECK(memcmp(a.s.peer, b.id.pk, NC_KEY_BYTES) != 0); /* what a pins or checks */
    uint8_t pkt[64] = {0}, out[64] = {0};
    size_t n = nc_seal(&b.s, 0, (const uint8_t *)"hi", 2, pkt, sizeof pkt);
    CHECK(n == 2 + NC_OVERHEAD && nc_open(&a.s, pkt, n, out, sizeof out, NULL) == -1);

    /* Malformed hellos and talking to oneself are refused. */
    nc_hello_make(&a.id, &a.eph, a.hello);
    nc_hello_make(&b.id, &b.eph, b.hello);
    memcpy(forged, b.hello, NC_HELLO_BYTES);
    forged[0] = 'X';
    CHECK(nc_session_derive(&a.s, &a.id, &a.eph, a.hello, forged) == -1);
    nc_hello_make(&a.id, &a.eph, a.hello);
    memcpy(forged, a.hello, NC_HELLO_BYTES);
    CHECK(nc_session_derive(&a.s, &a.id, &a.eph, a.hello, forged) == -1);
    /* An all-zero (low-order) ephemeral key gives no shared secret. */
    nc_hello_make(&a.id, &a.eph, a.hello);
    memcpy(forged, b.hello, NC_HELLO_BYTES);
    memset(forged + 4 + NC_KEY_BYTES, 0, NC_KEY_BYTES);
    CHECK(nc_session_derive(&a.s, &a.id, &a.eph, a.hello, forged) == -1);
    CHECK(memcmp(a.s.tx, zero, NC_KEY_BYTES) == 0);
}

static void test_packets(void)
{
    side a = {0}, b = {0};
    nc_keypair_new(&a.id);
    nc_keypair_new(&b.id);
    CHECK(pair(&a, &b));
    const char *msg = "hello over there";
    size_t len = strlen(msg);
    uint8_t pkt[8][64] = {{0}}, out[64] = {0};
    size_t n[8] = {0};
    for (int i = 0; i < 8; i++) n[i] = nc_seal(&a.s, 1, (const uint8_t *)msg, len, pkt[i], sizeof pkt[i]);
    CHECK(n[0] == len + NC_OVERHEAD);
    CHECK(memcmp(pkt[0], pkt[1], n[0]) != 0); /* fresh nonce per packet */

    int ch = -1;
    CHECK(nc_open(&b.s, pkt[0], n[0], out, sizeof out, &ch) == (long)len && ch == 1 && memcmp(out, msg, len) == 0);
    CHECK(nc_open(&b.s, pkt[0], n[0], out, sizeof out, NULL) == -1); /* replay */
    /* Out of order inside the window is fine, once each. */
    CHECK(nc_open(&b.s, pkt[3], n[3], out, sizeof out, NULL) == (long)len);
    CHECK(nc_open(&b.s, pkt[2], n[2], out, sizeof out, NULL) == (long)len);
    CHECK(nc_open(&b.s, pkt[2], n[2], out, sizeof out, NULL) == -1);

    /* Any flipped bit, in the header or the body, fails, and does not
     * use up the packet's slot. */
    for (size_t i = 0; i < n[4]; i++) {
        pkt[4][i] ^= 0x10;
        CHECK(nc_open(&b.s, pkt[4], n[4], out, sizeof out, NULL) == -1);
        pkt[4][i] ^= 0x10;
    }
    CHECK(nc_open(&b.s, pkt[4], n[4], out, sizeof out, NULL) == (long)len);
    CHECK(nc_open(&b.s, pkt[5], n[5] - 1, out, sizeof out, NULL) == -1); /* cut short */
    CHECK(nc_open(&b.s, pkt[5], 3, out, sizeof out, NULL) == -1);
    CHECK(nc_open(&b.s, pkt[5], n[5], out, 4, NULL) == -1); /* no room */
    /* A packet does not open in the direction it was sent from. */
    CHECK(nc_open(&a.s, pkt[5], n[5], out, sizeof out, NULL) == -1);
    CHECK(nc_open(&b.s, pkt[5], n[5], out, sizeof out, NULL) == (long)len);

    /* Channels count separately: a reliable message held back by resends
     * still opens after many packets on another channel. */
    uint8_t late[64] = {0}, fast[64] = {0};
    size_t ln = nc_seal(&a.s, 0, (const uint8_t *)"late", 4, late, sizeof late);
    for (int i = 0; i < 200; i++) {
        size_t fn = nc_seal(&a.s, 2, (const uint8_t *)"pos", 3, fast, sizeof fast);
        CHECK(nc_open(&b.s, fast, fn, out, sizeof out, NULL) == 3);
    }
    CHECK(nc_open(&b.s, late, ln, out, sizeof out, &ch) == 4 && ch == 0);
    /* Within one channel, more than 64 behind is refused. */
    size_t on = nc_seal(&a.s, 2, (const uint8_t *)"old", 3, late, sizeof late);
    for (int i = 0; i < 64; i++) {
        size_t fn = nc_seal(&a.s, 2, (const uint8_t *)"pos", 3, fast, sizeof fast);
        CHECK(nc_open(&b.s, fast, fn, out, sizeof out, NULL) == 3);
    }
    CHECK(nc_open(&b.s, late, on, out, sizeof out, NULL) == -1);
    /* Moving a packet to another channel breaks its tag. */
    on = nc_seal(&a.s, 2, (const uint8_t *)"mv", 2, late, sizeof late);
    late[0] = 3;
    CHECK(nc_open(&b.s, late, on, out, sizeof out, NULL) == -1);
    late[0] = 9;
    CHECK(nc_open(&b.s, late, on, out, sizeof out, NULL) == -1);

    CHECK(nc_seal(&a.s, NC_CHANNELS, (const uint8_t *)"x", 1, late, sizeof late) == 0);
    CHECK(nc_seal(&a.s, 0, (const uint8_t *)"x", 1, late, NC_OVERHEAD) == 0);
    CHECK(nc_seal(&a.s, 0, NULL, 0, late, NC_OVERHEAD) == NC_OVERHEAD); /* empty is allowed */
}

static void test_hex(void)
{
    uint8_t k[NC_KEY_BYTES] = {0}, back[NC_KEY_BYTES] = {0};
    for (int i = 0; i < NC_KEY_BYTES; i++) k[i] = (uint8_t)(i * 7 + 1);
    char hex[2 * NC_KEY_BYTES + 1] = {0};
    nc_key_hex(k, hex);
    CHECK(strlen(hex) == 64 && strncmp(hex, "01080f", 6) == 0);
    CHECK(nc_key_parse(hex, back) == 0 && memcmp(k, back, sizeof k) == 0);
    char bad[80] = {0};
    memcpy(bad, hex, 64);
    bad[10] = 'g';
    CHECK(nc_key_parse(bad, back) == -1);
    bad[10] = hex[10];
    bad[64] = '0';
    CHECK(nc_key_parse(bad, back) == -1); /* too long */
    CHECK(nc_key_parse("abcd", back) == -1);
}

void test_netcrypto_all(void)
{
    CHECK(nc_init() == 0);
    test_handshake();
    test_packets();
    test_hex();
}
