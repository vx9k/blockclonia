#include "net.h"
#include "log.h"
#include "mem.h"
#include "wire.h"

#include <enet/enet.h>
#include <stdio.h>
#include <string.h>

#define MAX_LINKS (NET_MAX_PLAYERS * 2)
#define HELLO_TIMEOUT 5.0 /* s for the other side's hello once connected */
#define CHAT_PENDING 4    /* lines held per player until the pair keys are ready */
#define CHAT_BURST 6.0    /* lines a player may send at once ... */
#define CHAT_RATE 1.0     /* ... refilled at this many per second */
#define QUEUE_LEN 64

enum { ROLE_JOIN = 1, ROLE_MESH = 2 };
enum { LINK_FREE, LINK_DIALING, LINK_HELLO, LINK_UP };
enum { MSG_HELLO = 1, MSG_WELCOME, MSG_PEER_JOINED, MSG_PEER_LEFT, MSG_ROUTE, MSG_PAIR_HELLO, MSG_CHAT, MSG_MESH_OK };
/* Sent as ENet's disconnect data, so the other side can say why. */
enum { BYE_NONE, BYE_FULL, BYE_NAME, BYE_VERSION, BYE_HANDSHAKE, BYE_STRANGER, BYE_LEAVING };

typedef struct net_link {
    ENetPeer *ep;
    int state, role, player; /* player -1 until known */
    int claim;               /* mesh link waiting for the host to announce this player */
    double since;
    nc_keypair eph;
    uint8_t mine[NC_HELLO_BYTES];
    nc_session s;
} net_link;

typedef struct {
    int present;
    char name[NET_NAME_MAX + 1];
    uint8_t pk[NC_KEY_BYTES];
    uint32_t ip; /* network byte order, as ENet keeps it */
    uint16_t port;
    net_link *direct;
    /* The pair's own keys, for end-to-end chat. */
    int pair_sent, pair_up;
    nc_keypair pair_eph;
    uint8_t pair_mine[NC_HELLO_BYTES];
    nc_session pair;
    char pending[CHAT_PENDING][NET_CHAT_MAX + 1];
    int pending_n;
    double tokens, tokens_t; /* chat rate limit for lines from this player */
} member;

struct net {
    ENetHost *h;
    nc_keypair id;
    char name[NET_NAME_MAX + 1];
    int host, me, ended, no_mesh;
    uint16_t port;
    member m[NET_MAX_PLAYERS];
    net_link links[MAX_LINKS];
    net_link *up; /* a player's link to the host */
    uint8_t expect[NC_KEY_BYTES];
    int has_expect;
    double now;
    net_event q[QUEUE_LEN];
    int qh, qn;
    uint8_t *rx, *tx;
    size_t rx_cap, tx_cap;
};

static int g_enet_users;

/* ---------------------------------------------------------------- checks */

int net_name_valid(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > NET_NAME_MAX || s[0] == ' ' || s[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                 c == '_' || c == '.';
        if (!ok) return 0;
    }
    return 1;
}

int net_chat_valid(const char *s, size_t len)
{
    if (len < 1 || len > NET_CHAT_MAX) return 0;
    const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < len;) {
        uint8_t c = p[i];
        if (c < 0x20 || c == 0x7f) return 0;
        if (c < 0x80) {
            i++;
            continue;
        }
        /* UTF-8: the shortest form only, no surrogates, nothing past U+10FFFF. */
        size_t extra = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
        if (!extra || extra >= len - i) return 0;
        uint8_t lo = 0x80, hi = 0xBF;
        if (c == 0xE0) lo = 0xA0;
        if (c == 0xED) hi = 0x9F;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;
        if (p[i + 1] < lo || p[i + 1] > hi) return 0;
        for (size_t k = 2; k <= extra; k++)
            if (p[i + k] < 0x80 || p[i + k] > 0xBF) return 0;
        i += extra + 1;
    }
    return 1;
}

/* ---------------------------------------------------------------- events */

static net_event *push(net *n, net_event_type type, int player)
{
    if (n->qn == QUEUE_LEN) {
        log_warn("net: event queue full, dropping an event");
        return NULL;
    }
    net_event *e = &n->q[(n->qh + n->qn++) % QUEUE_LEN];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->player = player;
    return e;
}

static void end_session(net *n, const char *why)
{
    if (n->ended) return;
    n->ended = 1;
    net_event *e = push(n, NET_EV_ENDED, n->me);
    if (e) snprintf(e->text, sizeof e->text, "%s", why);
    log_info("net: session ended: %s", why);
}

static const char *bye_text(uint32_t why)
{
    switch (why) {
    case BYE_FULL: return "the session is full";
    case BYE_NAME: return "that name is taken or not allowed";
    case BYE_VERSION: return "the host runs a different version";
    case BYE_HANDSHAKE: return "the handshake failed";
    case BYE_STRANGER: return "the host did not vouch for this player";
    case BYE_LEAVING: return "the host left";
    default: return "lost the connection to the host";
    }
}

/* ---------------------------------------------------------------- links */

static net_link *link_new(net *n, ENetPeer *ep, int role)
{
    for (int i = 0; i < MAX_LINKS; i++)
        if (n->links[i].state == LINK_FREE) {
            net_link *l = &n->links[i];
            memset(l, 0, sizeof *l);
            l->ep = ep;
            l->role = role;
            l->player = -1;
            l->state = LINK_DIALING;
            l->since = n->now;
            ep->data = l;
            return l;
        }
    return NULL;
}

static void link_free(net *n, net_link *l)
{
    for (int i = 0; i < NET_MAX_PLAYERS; i++)
        if (n->m[i].direct == l) n->m[i].direct = NULL;
    if (n->up == l) n->up = NULL;
    if (l->ep) l->ep->data = NULL;
    nc_wipe(l, sizeof *l);
    l->state = LINK_FREE;
}

static void link_drop(net *n, net_link *l, uint32_t why)
{
    if (l->ep) enet_peer_disconnect_now(l->ep, why);
    link_free(n, l);
}

static uint8_t *grow(uint8_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        *buf = mem_realloc(*buf, need);
        *cap = need;
    }
    return *buf;
}

/* Seals a message for a link and queues it. */
static int link_send(net *n, net_link *l, int ch, const uint8_t *data, size_t len)
{
    if (!l || l->state != LINK_UP) return -1;
    uint8_t *out = grow(&n->tx, &n->tx_cap, len + NC_OVERHEAD);
    size_t sealed = nc_seal(&l->s, ch, data, len, out, len + NC_OVERHEAD);
    if (!sealed) return -1;
    uint32_t flags = ch == NET_CH_STATE ? 0 : ENET_PACKET_FLAG_RELIABLE;
    ENetPacket *p = enet_packet_create(out, sealed, flags);
    if (!p || enet_peer_send(l->ep, (enet_uint8)ch, p) != 0) {
        if (p) enet_packet_destroy(p);
        return -1;
    }
    return 0;
}

static void send_hello(net *n, net_link *l)
{
    uint8_t buf[256] = {0};
    nc_hello_make(&n->id, &l->eph, l->mine);
    wire_w w = wire_writer(buf, sizeof buf);
    wire_u8(&w, MSG_HELLO);
    wire_u16(&w, NET_PROTOCOL);
    wire_u8(&w, (uint8_t)l->role);
    wire_bytes(&w, l->mine, NC_HELLO_BYTES);
    wire_str(&w, n->name, NET_NAME_MAX);
    wire_u8(&w, (uint8_t)(n->me < 0 ? 0xff : n->me));
    wire_u16(&w, n->port);
    ENetPacket *p = enet_packet_create(buf, w.len, ENET_PACKET_FLAG_RELIABLE);
    if (p && enet_peer_send(l->ep, NET_CH_CONTROL, p) != 0) enet_packet_destroy(p);
    l->state = LINK_HELLO;
    l->since = n->now;
}

/* ---------------------------------------------------------------- members */

static void member_clear(member *m) { nc_wipe(m, sizeof *m); }

static void member_set(net *n, int id, const char *name, const uint8_t *pk, uint32_t ip, uint16_t port)
{
    member *m = &n->m[id];
    member_clear(m);
    m->present = 1;
    snprintf(m->name, sizeof m->name, "%s", name);
    memcpy(m->pk, pk, NC_KEY_BYTES);
    m->ip = ip;
    m->port = port;
    m->tokens = CHAT_BURST;
    m->tokens_t = n->now;
}

static int name_taken(const net *n, const char *name)
{
    for (int i = 0; i < NET_MAX_PLAYERS; i++)
        if (n->m[i].present && strcmp(n->m[i].name, name) == 0) return 1;
    return 0;
}

static void put_member(wire_w *w, const net *n, int id)
{
    const member *m = &n->m[id];
    wire_u8(w, (uint8_t)id);
    wire_str(w, m->name, NET_NAME_MAX);
    wire_bytes(w, m->pk, NC_KEY_BYTES);
    wire_u32(w, m->ip);
    wire_u16(w, m->port);
}

/* Reads a member entry into scratch; the caller checks r->bad. */
static int get_member(wire_r *r, char *name, uint8_t *pk, uint32_t *ip, uint16_t *port)
{
    int id = wire_get_u8(r);
    wire_get_str(r, name, NET_NAME_MAX + 1);
    wire_get_bytes(r, pk, NC_KEY_BYTES);
    *ip = wire_get_u32(r);
    *port = wire_get_u16(r);
    if (id >= NET_MAX_PLAYERS || !net_name_valid(name)) r->bad = 1;
    return id;
}

/* Sends a chat-plane message (pair hello or sealed chat) to a player:
 * over the direct link when there is one, else through the host. */
static int send_pair(net *n, int to, const uint8_t *msg, size_t len)
{
    member *m = &n->m[to];
    if (m->direct && m->direct->state == LINK_UP) return link_send(n, m->direct, NET_CH_CHAT, msg, len);
    if (n->host || !n->up) return -1;
    uint8_t *buf = mem_alloc(len + 2);
    buf[0] = MSG_ROUTE;
    buf[1] = (uint8_t)to;
    memcpy(buf + 2, msg, len);
    int r = link_send(n, n->up, NET_CH_CONTROL, buf, len + 2);
    mem_free(buf);
    return r;
}

static void pair_start(net *n, int id)
{
    member *m = &n->m[id];
    if (id == n->me || !m->present || m->pair_sent) return;
    uint8_t msg[1 + NC_HELLO_BYTES] = {MSG_PAIR_HELLO};
    nc_hello_make(&n->id, &m->pair_eph, m->pair_mine);
    memcpy(msg + 1, m->pair_mine, NC_HELLO_BYTES);
    if (send_pair(n, id, msg, sizeof msg) == 0) m->pair_sent = 1;
}

static int chat_to(net *n, int id, const char *text)
{
    member *m = &n->m[id];
    size_t len = strlen(text);
    uint8_t msg[1 + NET_CHAT_MAX + NC_OVERHEAD] = {MSG_CHAT};
    size_t sealed = nc_seal(&m->pair, 0, (const uint8_t *)text, len, msg + 1, sizeof msg - 1);
    return sealed ? send_pair(n, id, msg, sealed + 1) : -1;
}

/* A chat-plane message from player `from`, direct or relayed. */
static void on_pair_msg(net *n, int from, const uint8_t *data, size_t len)
{
    if (from < 0 || from >= NET_MAX_PLAYERS || from == n->me || !n->m[from].present || len < 1) return;
    member *m = &n->m[from];
    if (data[0] == MSG_PAIR_HELLO && len == 1 + NC_HELLO_BYTES && !m->pair_up) {
        /* Only the identity the host vouched for may set up the pair. */
        if (memcmp(data + 1 + 4, m->pk, NC_KEY_BYTES) != 0) return;
        if (!m->pair_sent) pair_start(n, from);
        if (!m->pair_sent) return;
        if (nc_session_derive(&m->pair, &n->id, &m->pair_eph, m->pair_mine, data + 1) != 0) return;
        m->pair_up = 1;
        for (int i = 0; i < m->pending_n; i++) chat_to(n, from, m->pending[i]);
        m->pending_n = 0;
    } else if (data[0] == MSG_CHAT && m->pair_up) {
        char text[NET_CHAT_MAX + 1] = {0};
        long got = nc_open(&m->pair, data + 1, len - 1, (uint8_t *)text, NET_CHAT_MAX, NULL);
        if (got <= 0 || !net_chat_valid(text, (size_t)got)) return;
        text[got] = '\0';
        m->tokens += (n->now - m->tokens_t) * CHAT_RATE;
        if (m->tokens > CHAT_BURST) m->tokens = CHAT_BURST;
        m->tokens_t = n->now;
        if (m->tokens < 1.0) return; /* flooding: dropped */
        m->tokens -= 1.0;
        net_event *e = push(n, NET_EV_CHAT, from);
        if (e) memcpy(e->text, text, (size_t)got + 1);
    }
}

/* ---------------------------------------------------------------- host side */

static void host_admit(net *n, net_link *l, const char *name, uint16_t port)
{
    int id = -1;
    for (int i = 1; i < NET_MAX_PLAYERS && id < 0; i++)
        if (!n->m[i].present) id = i;
    if (id < 0) {
        link_drop(n, l, BYE_FULL);
        return;
    }
    if (!net_name_valid(name) || name_taken(n, name)) {
        link_drop(n, l, BYE_NAME);
        return;
    }
    member_set(n, id, name, l->s.peer, l->ep->address.host, port);
    n->m[id].direct = l;
    l->player = id;

    uint8_t buf[1 + 2 + NET_MAX_PLAYERS * 64] = {0};
    wire_w w = wire_writer(buf, sizeof buf);
    wire_u8(&w, MSG_WELCOME);
    wire_u8(&w, (uint8_t)id);
    int count = 0;
    for (int i = 1; i < NET_MAX_PLAYERS; i++) count += n->m[i].present && i != id;
    wire_u8(&w, (uint8_t)count);
    for (int i = 1; i < NET_MAX_PLAYERS; i++)
        if (n->m[i].present && i != id) put_member(&w, n, i);
    link_send(n, l, NET_CH_CONTROL, buf, w.len);

    w = wire_writer(buf, sizeof buf);
    wire_u8(&w, MSG_PEER_JOINED);
    put_member(&w, n, id);
    for (int i = 1; i < NET_MAX_PLAYERS; i++)
        if (n->m[i].present && i != id) link_send(n, n->m[i].direct, NET_CH_CONTROL, buf, w.len);
    log_info("net: %s joined as player %d", name, id);
    push(n, NET_EV_JOINED, id);
    pair_start(n, id);
}

static void host_player_left(net *n, int id)
{
    if (id <= 0 || id >= NET_MAX_PLAYERS || !n->m[id].present) return;
    log_info("net: %s left", n->m[id].name);
    member_clear(&n->m[id]);
    uint8_t msg[2] = {MSG_PEER_LEFT, (uint8_t)id};
    for (int i = 1; i < NET_MAX_PLAYERS; i++)
        if (n->m[i].present) link_send(n, n->m[i].direct, NET_CH_CONTROL, msg, sizeof msg);
    push(n, NET_EV_LEFT, id);
}

/* ---------------------------------------------------------------- mesh */

/* Takes a called mesh link into use once the caller is a player the host
 * announced, with the key it announced, and tells the caller, who then
 * sends chat over it instead of through the host. */
static void mesh_attach(net *n, net_link *l)
{
    int id = l->claim;
    if (id <= 0 || id >= NET_MAX_PLAYERS || id == n->me || !n->m[id].present) return; /* not yet announced */
    member *m = &n->m[id];
    if (memcmp(m->pk, l->s.peer, NC_KEY_BYTES) != 0 || (m->direct && m->direct != l)) {
        link_drop(n, l, BYE_STRANGER);
        return;
    }
    l->player = id;
    m->direct = l;
    const uint8_t ok = MSG_MESH_OK;
    link_send(n, l, NET_CH_CONTROL, &ok, 1);
}

/* ---------------------------------------------------------------- handshake */

static void on_hello(net *n, net_link *l, const uint8_t *data, size_t len)
{
    wire_r r = wire_reader(data, len);
    uint8_t theirs[NC_HELLO_BYTES] = {0};
    char name[NET_NAME_MAX + 1] = {0};
    wire_get_u8(&r);
    unsigned proto = wire_get_u16(&r);
    int role = wire_get_u8(&r);
    wire_get_bytes(&r, theirs, NC_HELLO_BYTES);
    wire_get_str(&r, name, sizeof name);
    int claimed = wire_get_u8(&r);
    uint16_t port = wire_get_u16(&r);
    if (r.bad || r.pos != len || role != l->role) {
        link_drop(n, l, BYE_HANDSHAKE);
        return;
    }
    if (proto != NET_PROTOCOL) {
        if (l == n->up) end_session(n, bye_text(BYE_VERSION));
        link_drop(n, l, BYE_VERSION);
        return;
    }
    if (nc_session_derive(&l->s, &n->id, &l->eph, l->mine, theirs) != 0) {
        if (l == n->up) end_session(n, bye_text(BYE_HANDSHAKE));
        link_drop(n, l, BYE_HANDSHAKE);
        return;
    }
    l->state = LINK_UP;

    if (n->host) {
        if (role == ROLE_JOIN) host_admit(n, l, name, port);
        else link_drop(n, l, BYE_STRANGER);
        return;
    }
    if (l == n->up) {
        if (n->has_expect && memcmp(l->s.peer, n->expect, NC_KEY_BYTES) != 0) {
            end_session(n, "the host's identity key changed since the last visit");
            link_drop(n, l, BYE_HANDSHAKE);
            return;
        }
        if (!net_name_valid(name)) {
            end_session(n, bye_text(BYE_HANDSHAKE));
            link_drop(n, l, BYE_HANDSHAKE);
            return;
        }
        member_set(n, 0, name, l->s.peer, l->ep->address.host, port);
        n->m[0].direct = l;
        l->player = 0;
        return;
    }
    /* A mesh link. The dialler already knows who it called; the side that
     * was called waits, if need be, for the host to announce the caller. */
    if (l->player >= 0) {
        const member *m = &n->m[l->player];
        if (!m->present || memcmp(m->pk, l->s.peer, NC_KEY_BYTES) != 0) link_drop(n, l, BYE_STRANGER);
        return;
    }
    l->claim = claimed;
    mesh_attach(n, l);
}

/* ---------------------------------------------------------------- control */

static void on_welcome(net *n, wire_r *r)
{
    int me = wire_get_u8(r);
    int count = wire_get_u8(r);
    struct {
        int id;
        char name[NET_NAME_MAX + 1];
        uint8_t pk[NC_KEY_BYTES];
        uint32_t ip;
        uint16_t port;
    } e[NET_MAX_PLAYERS] = {{0}};
    if (n->me >= 0 || me <= 0 || me >= NET_MAX_PLAYERS || count >= NET_MAX_PLAYERS) r->bad = 1;
    for (int i = 0; i < count && !r->bad; i++) {
        e[i].id = get_member(r, e[i].name, e[i].pk, &e[i].ip, &e[i].port);
        if (e[i].id == 0 || e[i].id == me) r->bad = 1;
    }
    if (r->bad || r->pos != r->len) return;
    n->me = me;
    member_set(n, me, n->name, n->id.pk, 0, n->port);
    push(n, NET_EV_JOINED, me);
    push(n, NET_EV_JOINED, 0);
    pair_start(n, 0);
    for (int i = 0; i < count; i++) {
        member_set(n, e[i].id, e[i].name, e[i].pk, e[i].ip, e[i].port);
        push(n, NET_EV_JOINED, e[i].id);
        if (!n->no_mesh && e[i].port) {
            ENetAddress a = {.host = e[i].ip, .port = e[i].port};
            ENetPeer *ep = enet_host_connect(n->h, &a, NET_CHANNELS, 0);
            net_link *l = ep ? link_new(n, ep, ROLE_MESH) : NULL;
            if (l) l->player = e[i].id;
            else if (ep) enet_peer_reset(ep);
        }
        pair_start(n, e[i].id);
    }
    log_info("net: joined as player %d with %d others", me, count + 1);
}

static void on_control(net *n, net_link *l, const uint8_t *data, size_t len)
{
    if (len < 1) return;
    wire_r r = wire_reader(data, len);
    int type = wire_get_u8(&r);
    if (type == MSG_ROUTE && len >= 3) {
        int other = data[1];
        if (other >= NET_MAX_PLAYERS) return;
        if (n->host) {
            /* Relay: the host writes in who it came from, so a player
             * cannot pretend to be another. */
            if (other == 0) {
                on_pair_msg(n, l->player, data + 2, len - 2);
            } else if (n->m[other].present && other != l->player) {
                uint8_t *buf = mem_alloc(len);
                memcpy(buf, data, len);
                buf[1] = (uint8_t)l->player;
                link_send(n, n->m[other].direct, NET_CH_CONTROL, buf, len);
                mem_free(buf);
            }
        } else if (l == n->up) {
            on_pair_msg(n, other, data + 2, len - 2);
        }
        return;
    }
    if (type == MSG_MESH_OK && len == 1 && l->role == ROLE_MESH && l->player > 0) {
        member *m = &n->m[l->player];
        if (m->present && !m->direct && memcmp(m->pk, l->s.peer, NC_KEY_BYTES) == 0) m->direct = l;
        return;
    }
    if (n->host || l != n->up) return; /* the rest only comes from the host */
    if (type == MSG_WELCOME) {
        on_welcome(n, &r);
    } else if (type == MSG_PEER_JOINED && n->me >= 0) {
        char name[NET_NAME_MAX + 1] = {0};
        uint8_t pk[NC_KEY_BYTES] = {0};
        uint32_t ip = 0;
        uint16_t port = 0;
        int id = get_member(&r, name, pk, &ip, &port);
        if (r.bad || r.pos != len || id == 0 || id == n->me) return;
        member_set(n, id, name, pk, ip, port);
        push(n, NET_EV_JOINED, id);
        for (int i = 0; i < MAX_LINKS; i++)
            if (n->links[i].state == LINK_UP && n->links[i].role == ROLE_MESH && n->links[i].player < 0 &&
                n->links[i].claim == id)
                mesh_attach(n, &n->links[i]);
        pair_start(n, id);
    } else if (type == MSG_PEER_LEFT && len == 2) {
        int id = data[1];
        if (id <= 0 || id >= NET_MAX_PLAYERS || id == n->me || !n->m[id].present) return;
        if (n->m[id].direct) link_drop(n, n->m[id].direct, BYE_NONE);
        member_clear(&n->m[id]);
        push(n, NET_EV_LEFT, id);
    }
}

/* ---------------------------------------------------------------- ENet events */

/* Returns 1 if the packet became an event for the game in ev. */
static int on_receive(net *n, net_link *l, int ch, const uint8_t *data, size_t len, net_event *ev)
{
    if (l->state == LINK_HELLO) {
        if (ch == NET_CH_CONTROL && len > 0 && data[0] == MSG_HELLO) on_hello(n, l, data, len);
        else link_drop(n, l, BYE_HANDSHAKE);
        return 0;
    }
    if (l->state != LINK_UP || len < NC_OVERHEAD) return 0;
    uint8_t *out = grow(&n->rx, &n->rx_cap, len);
    int sealed_ch = -1;
    long got = nc_open(&l->s, data, len, out, len, &sealed_ch);
    if (got < 0 || sealed_ch != ch) return 0; /* forged, replayed or moved: dropped */
    if (ch == NET_CH_CONTROL) {
        on_control(n, l, out, (size_t)got);
    } else if (ch == NET_CH_CHAT) {
        on_pair_msg(n, l->player, out, (size_t)got);
    } else if (l->player >= 0 && (n->host ? l->role == ROLE_JOIN : l == n->up)) {
        memset(ev, 0, sizeof *ev);
        ev->type = NET_EV_MESSAGE;
        ev->player = l->player;
        ev->ch = ch;
        ev->data = out;
        ev->len = (size_t)got;
        return 1;
    }
    return 0;
}

static void on_disconnect(net *n, net_link *l, uint32_t why)
{
    if (l == n->up) {
        end_session(n, bye_text(why));
    } else if (n->host && l->role == ROLE_JOIN && l->player > 0 && n->m[l->player].direct == l) {
        host_player_left(n, l->player);
    }
    link_free(n, l);
}

static void expire_hellos(net *n)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        net_link *l = &n->links[i];
        int waiting = l->state == LINK_HELLO || (l->state == LINK_UP && l->role == ROLE_MESH && l->player < 0);
        if (waiting && n->now - l->since > HELLO_TIMEOUT) {
            if (l == n->up) end_session(n, bye_text(BYE_HANDSHAKE));
            link_drop(n, l, BYE_HANDSHAKE);
        }
    }
}

int net_poll(net *n, double now, net_event *ev)
{
    n->now = now;
    for (;;) {
        if (n->qn) {
            *ev = n->q[n->qh];
            n->qh = (n->qh + 1) % QUEUE_LEN;
            n->qn--;
            return 1;
        }
        ENetEvent e;
        memset(&e, 0, sizeof e);
        if (!n->h || enet_host_service(n->h, &e, 0) <= 0) {
            expire_hellos(n);
            if (!n->qn) return 0;
            continue;
        }
        net_link *l = e.peer ? (net_link *)e.peer->data : NULL;
        int got = 0;
        switch (e.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (!l) l = link_new(n, e.peer, n->host ? ROLE_JOIN : ROLE_MESH);
            if (!l) enet_peer_disconnect_now(e.peer, BYE_FULL);
            else send_hello(n, l);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            if (l) got = on_receive(n, l, e.channelID, e.packet->data, e.packet->dataLength, ev);
            enet_packet_destroy(e.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            if (l) on_disconnect(n, l, e.data);
            break;
        default: break;
        }
        if (got) return 1;
    }
}

void net_flush(net *n)
{
    if (n && n->h) enet_host_flush(n->h);
}

/* ---------------------------------------------------------------- setup */

static net *net_new(const nc_keypair *id, const char *name, uint16_t port)
{
    if (!net_name_valid(name)) {
        log_error("net: '%s' is not a valid player name", name);
        return NULL;
    }
    if (nc_init() != 0 || (g_enet_users == 0 && enet_initialize() != 0)) {
        log_error("net: cannot start the network library");
        return NULL;
    }
    g_enet_users++;
    net *n = mem_calloc(1, sizeof *n);
    n->id = *id;
    snprintf(n->name, sizeof n->name, "%s", name);
    n->me = -1;
    ENetAddress a = {.host = ENET_HOST_ANY, .port = port};
    n->h = enet_host_create(&a, MAX_LINKS, NET_CHANNELS, 0, 0);
    if (!n->h) {
        log_error("net: cannot listen on UDP port %u (in use?)", (unsigned)port);
        net_destroy(n);
        return NULL;
    }
    ENetAddress bound = {0};
    n->port = enet_socket_get_address(n->h->socket, &bound) == 0 ? bound.port : port;
    return n;
}

net *net_host(const nc_keypair *id, const char *name, uint16_t port)
{
    net *n = net_new(id, name, port ? port : NET_PORT);
    if (!n) return NULL;
    n->host = 1;
    n->me = 0;
    member_set(n, 0, name, id->pk, 0, n->port);
    log_info("net: hosting on UDP port %u", (unsigned)n->port);
    return n;
}

net *net_join(const nc_keypair *id, const char *name, const char *addr, uint16_t port, uint16_t listen_port,
              const uint8_t *expect_host)
{
    net *n = net_new(id, name, listen_port);
    if (!n) return NULL;
    if (expect_host) {
        memcpy(n->expect, expect_host, NC_KEY_BYTES);
        n->has_expect = 1;
    }
    ENetAddress a = {0};
    a.port = port ? port : NET_PORT;
    ENetPeer *ep = enet_address_set_host(&a, addr) == 0 ? enet_host_connect(n->h, &a, NET_CHANNELS, 0) : NULL;
    n->up = ep ? link_new(n, ep, ROLE_JOIN) : NULL;
    if (!n->up) {
        log_error("net: cannot reach '%s'", addr);
        net_destroy(n);
        return NULL;
    }
    log_info("net: joining %s:%u", addr, (unsigned)a.port);
    return n;
}

void net_destroy(net *n)
{
    if (!n) return;
    if (n->h) {
        for (int i = 0; i < MAX_LINKS; i++)
            if (n->links[i].state != LINK_FREE && n->links[i].ep)
                enet_peer_disconnect_now(n->links[i].ep, n->host ? BYE_LEAVING : BYE_NONE);
        enet_host_flush(n->h);
        enet_host_destroy(n->h);
    }
    mem_free(n->rx);
    mem_free(n->tx);
    nc_wipe(n, sizeof *n);
    mem_free(n);
    if (--g_enet_users == 0) enet_deinitialize();
}

/* ---------------------------------------------------------------- sending */

int net_send(net *n, int player, int ch, const uint8_t *data, size_t len)
{
    if (ch != NET_CH_GAME && ch != NET_CH_STATE) return -1;
    if (n->host) {
        if (player <= 0 || player >= NET_MAX_PLAYERS || !n->m[player].present) return -1;
        return link_send(n, n->m[player].direct, ch, data, len);
    }
    return player == 0 && n->me >= 0 ? link_send(n, n->up, ch, data, len) : -1;
}

void net_broadcast(net *n, int ch, const uint8_t *data, size_t len)
{
    if (!n->host) return;
    for (int i = 1; i < NET_MAX_PLAYERS; i++)
        if (n->m[i].present) net_send(n, i, ch, data, len);
}

int net_chat(net *n, const char *text)
{
    if (!net_chat_valid(text, strlen(text)) || n->me < 0) return 0;
    int sent = 0;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        member *m = &n->m[i];
        if (i == n->me || !m->present) continue;
        if (m->pair_up) {
            sent += chat_to(n, i, text) == 0;
        } else if (m->pending_n < CHAT_PENDING) {
            snprintf(m->pending[m->pending_n++], sizeof m->pending[0], "%s", text);
            sent++;
        }
    }
    return sent;
}

/* ---------------------------------------------------------------- queries */

int net_is_host(const net *n) { return n->host; }
int net_me(const net *n) { return n->me; }
uint16_t net_port(const net *n) { return n->port; }
void net_test_no_mesh(net *n) { n->no_mesh = 1; }

int net_player_present(const net *n, int player)
{ return player >= 0 && player < NET_MAX_PLAYERS && n->m[player].present; }

const char *net_player_name(const net *n, int player) { return net_player_present(n, player) ? n->m[player].name : ""; }

const uint8_t *net_host_key(const net *n)
{
    if (n->host) return n->id.pk;
    return n->up && n->up->state == LINK_UP ? n->up->s.peer : NULL;
}

int net_player_direct(const net *n, int player)
{ return net_player_present(n, player) && n->m[player].direct && n->m[player].direct->state == LINK_UP; }
