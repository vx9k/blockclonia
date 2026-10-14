/* The multiplayer transport (net.c) over loopback UDP: joining, the mesh,
 * end-to-end chat directly and through the host, game messages, names
 * and pinned host keys refused, and players and the host leaving. */
#include "net.h"
#include "netkeys.h"
#include "os.h"
#include "save.h"
#include "wire.h"

#include "test_util.h"
#include <enet/enet.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXEV 64

typedef struct {
    net *n;
    net_event ev[MAXEV];
    uint8_t data[MAXEV][64];
    int count;
} peer;

static void drain(peer *p)
{
    net_event e;
    while (p->n && net_poll(p->n, os_now(), &e)) {
        if (p->count == MAXEV) continue;
        if (e.type == NET_EV_MESSAGE) {
            size_t n = e.len < sizeof p->data[0] ? e.len : sizeof p->data[0];
            memcpy(p->data[p->count], e.data, n);
            e.data = p->data[p->count];
        }
        p->ev[p->count++] = e;
    }
    if (p->n) net_flush(p->n);
}

static void pump(peer *ps, int count, double seconds)
{
    double end = os_now() + seconds;
    while (os_now() < end) {
        for (int i = 0; i < count; i++) drain(&ps[i]);
        os_sleep(0.001);
    }
}

static const net_event *find(const peer *p, net_event_type type, int who)
{
    for (int i = 0; i < p->count; i++)
        if (p->ev[i].type == type && (who < 0 || p->ev[i].player == who)) return &p->ev[i];
    return NULL;
}

/* Pumps until `want` sees an event, at most a few seconds. */
static const net_event *wait_for(peer *ps, int count, const peer *want, net_event_type type, int who)
{
    for (int i = 0; i < 3000; i++) {
        const net_event *e = find(want, type, who);
        if (e) return e;
        pump(ps, count, 0.002);
    }
    return find(want, type, who);
}

static void test_names(void)
{
    CHECK(net_name_valid("Steve") && net_name_valid("a.b-c_d 9"));
    CHECK(!net_name_valid("") && !net_name_valid(" lead") && !net_name_valid("trail "));
    CHECK(!net_name_valid("12345678901234567") && !net_name_valid("tab\t") && !net_name_valid("caf\xc3\xa9"));

    CHECK(net_chat_valid("hello", 5) && net_chat_valid("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x99\x82", 14));
    CHECK(!net_chat_valid("", 0) && !net_chat_valid("a\nb", 3) && !net_chat_valid("\x7f", 1));
    CHECK(!net_chat_valid("\xc3", 1));             /* cut short */
    CHECK(!net_chat_valid("\xc0\xaf", 2));         /* overlong */
    CHECK(!net_chat_valid("\xed\xa0\x80", 3));     /* a surrogate */
    CHECK(!net_chat_valid("\xf4\x90\x80\x80", 4)); /* past U+10FFFF */
    CHECK(!net_chat_valid("\xe2\x82", 2) && !net_chat_valid("\xe2\x28\xac", 3));
    char longer[NET_CHAT_MAX + 2] = {0};
    memset(longer, 'a', NET_CHAT_MAX + 1);
    CHECK(net_chat_valid(longer, NET_CHAT_MAX) && !net_chat_valid(longer, NET_CHAT_MAX + 1));
}

static void test_session(void)
{
    nc_keypair kh = {0}, ka = {0}, kb = {0}, kc = {0};
    nc_keypair_new(&kh);
    nc_keypair_new(&ka);
    nc_keypair_new(&kb);
    nc_keypair_new(&kc);
    /* A port of our own, so parallel runs do not collide. */
    static peer p[5];
    memset(p, 0, sizeof p);
    uint16_t port = 0;
    for (int tries = 0; tries < 20 && !p[0].n; tries++) {
        port = (uint16_t)(30000 + (getpid() * 7 + tries * 131) % 30000);
        p[0].n = net_host(&kh, "Host", port);
    }
    CHECK(p[0].n != NULL);
    if (!p[0].n) return;
    CHECK(net_is_host(p[0].n) && net_me(p[0].n) == 0 && net_port(p[0].n) == port);

    p[1].n = net_join(&ka, "Alice", "127.0.0.1", port, 0, NULL);
    CHECK(p[1].n != NULL);
    const net_event *e = wait_for(p, 2, &p[1], NET_EV_JOINED, 1);
    CHECK(e != NULL && net_me(p[1].n) == 1);
    CHECK(find(&p[0], NET_EV_JOINED, 1) != NULL);
    CHECK(strcmp(net_player_name(p[1].n, 0), "Host") == 0 && strcmp(net_player_name(p[0].n, 1), "Alice") == 0);
    CHECK(net_host_key(p[1].n) && memcmp(net_host_key(p[1].n), kh.pk, NC_KEY_BYTES) == 0);

    /* Bob arrives: Alice hears of him, he dials her directly. */
    p[2].n = net_join(&kb, "Bob", "127.0.0.1", port, 0, kh.pk); /* pinned key matches */
    CHECK(wait_for(p, 3, &p[2], NET_EV_JOINED, 2) != NULL);
    CHECK(wait_for(p, 3, &p[1], NET_EV_JOINED, 2) != NULL);
    CHECK(find(&p[2], NET_EV_JOINED, 1) != NULL && strcmp(net_player_name(p[2].n, 1), "Alice") == 0);
    pump(p, 3, 0.2); /* pair keys and the mesh link settle */
    CHECK(net_player_direct(p[2].n, 1) && net_player_direct(p[1].n, 2)); /* the mesh link is up */
    CHECK(net_player_direct(p[2].n, 0) && net_player_direct(p[0].n, 2));

    /* Chat reaches everyone else, from the right player. */
    CHECK(net_chat(p[1].n, "hi from Alice") == 2);
    e = wait_for(p, 3, &p[2], NET_EV_CHAT, 1);
    CHECK(e && strcmp(e->text, "hi from Alice") == 0);
    e = wait_for(p, 3, &p[0], NET_EV_CHAT, 1);
    CHECK(e && strcmp(e->text, "hi from Alice") == 0);
    CHECK(find(&p[1], NET_EV_CHAT, -1) == NULL); /* not echoed */
    CHECK(net_chat(p[1].n, "bad\nline") == 0);

    /* Game messages go between the host and each player. */
    CHECK(net_send(p[1].n, 0, NET_CH_GAME, (const uint8_t *)"abc", 3) == 0);
    CHECK(net_send(p[1].n, 2, NET_CH_GAME, (const uint8_t *)"abc", 3) == -1); /* only to the host */
    CHECK(net_send(p[1].n, 0, NET_CH_CHAT, (const uint8_t *)"abc", 3) == -1);
    e = wait_for(p, 3, &p[0], NET_EV_MESSAGE, 1);
    CHECK(e && e->ch == NET_CH_GAME && e->len == 3 && memcmp(e->data, "abc", 3) == 0);
    net_broadcast(p[0].n, NET_CH_STATE, (const uint8_t *)"snap", 4);
    e = wait_for(p, 3, &p[2], NET_EV_MESSAGE, 0);
    CHECK(e && e->ch == NET_CH_STATE && e->len == 4);
    CHECK(wait_for(p, 3, &p[1], NET_EV_MESSAGE, 0) != NULL);

    /* A taken name and a host key that changed end the joiner's session. */
    p[3].n = net_join(&kc, "Alice", "127.0.0.1", port, 0, NULL);
    e = wait_for(p, 4, &p[3], NET_EV_ENDED, -1);
    CHECK(e && strstr(e->text, "name") != NULL);
    net_destroy(p[3].n);
    memset(&p[3], 0, sizeof p[3]);
    p[3].n = net_join(&kc, "Carol", "127.0.0.1", port, 0, ka.pk);
    e = wait_for(p, 4, &p[3], NET_EV_ENDED, -1);
    CHECK(e && strstr(e->text, "identity key") != NULL);
    net_destroy(p[3].n);
    memset(&p[3], 0, sizeof p[3]);
    /* A second copy of the host's own game (one settings folder, one key)
     * is told why it cannot join, not just that the handshake failed. */
    p[3].n = net_join(&kh, "Twin", "127.0.0.1", port, 0, NULL);
    e = wait_for(p, 4, &p[3], NET_EV_ENDED, -1);
    CHECK(e && strstr(e->text, "--config") != NULL);
    net_destroy(p[3].n);
    memset(&p[3], 0, sizeof p[3]);

    /* Carol cannot reach anyone directly: her chat goes through the host,
     * sealed with keys the host does not have. */
    p[3].n = net_join(&kc, "Carol", "127.0.0.1", port, 0, NULL);
    net_test_no_mesh(p[3].n);
    CHECK(wait_for(p, 4, &p[3], NET_EV_JOINED, 3) != NULL);
    pump(p, 4, 0.2);
    CHECK(!net_player_direct(p[3].n, 2) && !net_player_direct(p[2].n, 3) && net_player_direct(p[3].n, 0));
    CHECK(net_chat(p[3].n, "relayed \xe2\x9c\x93") == 3);
    e = wait_for(p, 4, &p[2], NET_EV_CHAT, 3);
    CHECK(e && strcmp(e->text, "relayed \xe2\x9c\x93") == 0);
    CHECK(net_chat(p[2].n, "back to Carol") == 3);
    e = wait_for(p, 4, &p[3], NET_EV_CHAT, 2);
    CHECK(e && strcmp(e->text, "back to Carol") == 0);

    /* Alice leaves: everyone hears it. Then the host leaves. */
    net_destroy(p[1].n);
    memset(&p[1], 0, sizeof p[1]);
    CHECK(wait_for(p, 4, &p[0], NET_EV_LEFT, 1) != NULL);
    CHECK(wait_for(p, 4, &p[2], NET_EV_LEFT, 1) != NULL);
    CHECK(!net_player_present(p[2].n, 1));
    net_destroy(p[0].n);
    memset(&p[0], 0, sizeof p[0]);
    e = wait_for(p, 4, &p[2], NET_EV_ENDED, -1);
    CHECK(e && strstr(e->text, "host left") != NULL);
    net_destroy(p[2].n);
    net_destroy(p[3].n);
}

static void test_keys(void)
{
    char dir[256] = {0}, id[300] = {0}, hosts[300] = {0};
    CHECK(tmp_dir_make(dir, sizeof dir, "mc_keys_"));
    snprintf(id, sizeof id, "%s/identity.key", dir);
    snprintf(hosts, sizeof hosts, "%s/known_hosts", dir);

    /* Made once, readable by this user only, the same key after. */
    nc_keypair a = {0}, b = {0};
    CHECK(netkeys_identity(id, &a) == 1);
    struct stat st;
    CHECK(stat(id, &st) == 0 && (st.st_mode & 077) == 0);
    CHECK(netkeys_identity(id, &b) == 0 && memcmp(a.pk, b.pk, NC_KEY_BYTES) == 0);
    CHECK(memcmp(a.sk, b.sk, NC_KEY_BYTES) == 0);
    /* A damaged file is not used, and not overwritten either. */
    CHECK(save_write_file(id, "blockclonia identity v1\nnot hex\n", 32) == 0);
    CHECK(netkeys_identity(id, &b) == -1 && memcmp(a.pk, b.pk, NC_KEY_BYTES) != 0);
    char back[64] = {0};
    CHECK(save_read_file(id, back, sizeof back) == 32);
    /* A file that exists but cannot be read (here a directory) is not
     * replaced either. */
    char odd[300] = {0};
    snprintf(odd, sizeof odd, "%s/odd", dir);
    CHECK(os_mkdir(odd, 0700) == 0);
    CHECK(netkeys_identity(odd, &b) == -1 && os_is_dir(odd));
    uint8_t none[NC_KEY_BYTES] = {0};
    CHECK(netkeys_known(odd, "a:1", none) == -1 && netkeys_pin(odd, "a:1", a.pk) == -1 && os_is_dir(odd));
    tmp_remove(odd);

    /* Pinning: unknown, pinned, re-pinned, others kept; junk skipped. */
    uint8_t pk[NC_KEY_BYTES] = {0};
    CHECK(netkeys_known(hosts, "10.0.0.1:27450", pk) == 0);
    CHECK(netkeys_pin(hosts, "10.0.0.1:27450", a.pk) == 0);
    CHECK(netkeys_pin(hosts, "example.org:1", b.pk) == 0);
    CHECK(netkeys_known(hosts, "10.0.0.1:27450", pk) == 1 && memcmp(pk, a.pk, NC_KEY_BYTES) == 0);
    CHECK(netkeys_pin(hosts, "10.0.0.1:27450", b.pk) == 0);
    CHECK(netkeys_known(hosts, "10.0.0.1:27450", pk) == 1 && memcmp(pk, b.pk, NC_KEY_BYTES) == 0);
    CHECK(netkeys_known(hosts, "example.org:1", pk) == 1);
    CHECK(netkeys_pin(hosts, "has space", a.pk) == -1 && netkeys_pin(hosts, "", a.pk) == -1);
    char text[512] = {0};
    long n = save_read_file(hosts, text, sizeof text - 64);
    CHECK(n > 0);
    memcpy(text + n, "junk line\nevil.host zz\n", 23);
    CHECK(save_write_file(hosts, text, (size_t)n + 23) == 0);
    CHECK(netkeys_known(hosts, "example.org:1", pk) == 1 && netkeys_known(hosts, "junk", pk) == 0);
    CHECK(netkeys_pin(hosts, "new.host:2", a.pk) == 0);
    n = save_read_file(hosts, text, sizeof text - 1);
    text[n > 0 ? n : 0] = '\0';
    CHECK(strstr(text, "junk") == NULL && strstr(text, "example.org:1") != NULL); /* rewritten clean */
    tmp_remove(id);
    tmp_remove(hosts);
    tmp_remove(dir);

    /* Safety numbers: the same from both ends, different for another key,
     * and laid out as six groups of five digits. */
    char ab[NC_SAFETY_CHARS] = {0}, ba[NC_SAFETY_CHARS] = {0}, ac[NC_SAFETY_CHARS] = {0};
    nc_keypair c = {0};
    nc_keypair_new(&c);
    nc_safety_number(a.pk, c.pk, ab);
    nc_safety_number(c.pk, a.pk, ba);
    nc_safety_number(b.pk, c.pk, ac);
    CHECK(strcmp(ab, ba) == 0 && strcmp(ab, ac) != 0);
    int shape = strlen(ab) == NC_SAFETY_CHARS - 1;
    for (int i = 0; i < NC_SAFETY_CHARS - 1; i++) shape &= i % 6 == 5 ? ab[i] == ' ' : (ab[i] >= '0' && ab[i] <= '9');
    CHECK(shape);
}

/* Someone who knows only Bob's public key dials Alice claiming to be
 * Bob before Bob arrives. Without the sealed identity message the forged
 * link was taken into use and Bob's real one refused, which cut their
 * chat. Now it goes nowhere. */
static void test_forged_mesh(void)
{
    nc_keypair kh = {0}, ka = {0}, kb = {0};
    nc_keypair_new(&kh);
    nc_keypair_new(&ka);
    nc_keypair_new(&kb);
    static peer p[3];
    memset(p, 0, sizeof p);
    uint16_t port = 0;
    for (int tries = 0; tries < 20 && !p[0].n; tries++) {
        port = (uint16_t)(30000 + (getpid() * 11 + tries * 151 + 3) % 30000);
        p[0].n = net_host(&kh, "Host", port);
    }
    CHECK(p[0].n != NULL);
    if (!p[0].n) return;
    p[1].n = net_join(&ka, "Alice", "127.0.0.1", port, 0, NULL);
    CHECK(wait_for(p, 2, &p[1], NET_EV_JOINED, 1) != NULL);

    ENetHost *atk = enet_host_create(NULL, 1, NET_CHANNELS, 0, 0);
    ENetAddress a = {.host = 0, .port = net_port(p[1].n)};
    enet_address_set_host(&a, "127.0.0.1");
    CHECK(atk && enet_host_connect(atk, &a, NET_CHANNELS, 0));
    for (int i = 0; i < 400; i++) {
        ENetEvent e;
        while (atk && enet_host_service(atk, &e, 0) > 0) {
            if (e.type == ENET_EVENT_TYPE_CONNECT) {
                nc_keypair fake = {0}, eph = {0};
                uint8_t hello[NC_HELLO_BYTES] = {0}, buf[128] = {0};
                nc_keypair_new(&fake);
                memcpy(fake.pk, kb.pk, NC_KEY_BYTES); /* Bob's public key, not his secret */
                nc_hello_make(&fake, &eph, hello);
                wire_w w = wire_writer(buf, sizeof buf);
                wire_u8(&w, 1); /* hello */
                wire_u16(&w, NET_PROTOCOL);
                wire_u8(&w, 2); /* a mesh link */
                wire_bytes(&w, hello, sizeof hello);
                enet_peer_send(e.peer, NET_CH_CONTROL, enet_packet_create(buf, w.len, ENET_PACKET_FLAG_RELIABLE));
            } else if (e.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(e.packet);
            }
        }
        if (atk) enet_host_flush(atk);
        pump(p, 2, 0.001);
    }
    p[2].n = net_join(&kb, "Bob", "127.0.0.1", port, 0, NULL);
    CHECK(wait_for(p, 3, &p[1], NET_EV_JOINED, 2) != NULL);
    pump(p, 3, 0.3);
    CHECK(net_player_direct(p[1].n, 2) && net_player_direct(p[2].n, 1));
    CHECK(net_chat(p[2].n, "really Bob") == 2);
    const net_event *e = wait_for(p, 3, &p[1], NET_EV_CHAT, 2);
    CHECK(e && strcmp(e->text, "really Bob") == 0);
    if (atk) enet_host_destroy(atk);
    for (int i = 2; i >= 0; i--) net_destroy(p[i].n);
}

void test_net_all(void)
{
    CHECK(nc_init() == 0);
    test_names();
    test_keys();
    test_session();
    test_forged_mesh();
}
