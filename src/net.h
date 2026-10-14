/* Peer-to-peer multiplayer transport over ENet (UDP).
 *
 * One player hosts the session and is its authority: player 0, the one
 * the game state comes from. Others join by its address. The host gives
 * each joiner a player id and the names, identity keys and addresses of
 * everyone else, and the joiner dials each of them directly, so every
 * pair of players ends up with a link of its own (a full mesh). A player
 * accepts a mesh link only from someone the host announced, with the
 * identity key the host announced; the dialler checks the same.
 *
 * Every link starts with a cleartext hello (netcrypto.h) and seals all
 * later packets with ChaCha20-Poly1305. Chat is end-to-end on top of
 * that: each pair of players runs its own handshake and seals chat with
 * the pair's keys, then sends it over their direct link, or through the
 * host as opaque bytes when routers keep the two from reaching each
 * other. The host can drop chat it relays but cannot read or forge it.
 *
 * Game messages go only between the host and each player, on two
 * channels: NET_CH_GAME (reliable, in order) and NET_CH_STATE
 * (unreliable, newest wins). Their contents are the game's business.
 *
 * Not here: deciding whether the host's identity key is the one seen
 * last time (the caller pins it and passes it in), files, NAT traversal
 * (peers need a reachable address: LAN, forwarded port or VPN), IPv6
 * (ENet 1.3 is IPv4 only) and handing the authority to another player
 * when the host leaves (the session ends). */
#ifndef MC_NET_H
#define MC_NET_H

#include "netcrypto.h"
#include <stddef.h>
#include <stdint.h>

#define NET_PORT 27450
#define NET_MAX_PLAYERS 8
#define NET_NAME_MAX 16
#define NET_CHAT_MAX 200      /* bytes of UTF-8 per chat line */
#define NET_PROTOCOL 4         /* bump when peers would disagree: messages, block or item ids */
/* The largest game message: a column in the save format with a header. */
#define NET_MAX_MESSAGE (4 * 16 * 128 * 16 + 1024)

enum { NET_CH_CONTROL, NET_CH_GAME, NET_CH_STATE, NET_CH_CHAT, NET_CHANNELS };
_Static_assert(NET_CHANNELS <= NC_CHANNELS, "each channel needs its own nonce counter");

typedef enum {
    NET_EV_NONE,
    NET_EV_JOINED,   /* player joined (host: also when they join) */
    NET_EV_LEFT,     /* player left */
    NET_EV_MESSAGE,  /* data from player on channel ch (host <-> player only) */
    NET_EV_CHAT,     /* text from player, end-to-end */
    NET_EV_ENDED,    /* this side's session is over: text says why */
} net_event_type;

typedef struct {
    net_event_type type;
    int player;
    int ch;
    const uint8_t *data; /* NET_EV_MESSAGE: valid until the next net_poll */
    size_t len;
    char text[NET_CHAT_MAX + 1]; /* NET_EV_CHAT: the line; NET_EV_ENDED: the reason */
} net_event;

typedef struct net net;

/* A name a player may use: 1..NET_NAME_MAX of letters, digits, space,
 * '-', '_' and '.', not starting or ending with a space. */
int net_name_valid(const char *s);
/* Chat text that may be sent: 1..NET_CHAT_MAX bytes of valid UTF-8
 * without control characters. */
int net_chat_valid(const char *s, size_t len);

/* Hosts a session on UDP port (0: NET_PORT). NULL if the port is taken or
 * the name is invalid; the reason is logged. */
net *net_host(const nc_keypair *id, const char *name, uint16_t port);
/* Joins the session at addr:port. listen_port is where other players
 * dial this one (0: any free port; they reach it only on a LAN then).
 * expect_host: the host's identity key from an earlier visit, or NULL on
 * first contact; a different key ends the session (NET_EV_ENDED). The
 * outcome arrives through net_poll. */
net *net_join(const nc_keypair *id, const char *name, const char *addr, uint16_t port, uint16_t listen_port,
              const uint8_t *expect_host);
/* Leaves politely and frees everything. NULL is fine. */
void net_destroy(net *n);

/* The next event, or 0 when nothing is waiting. Never blocks. now is a
 * monotonic clock in seconds, for timeouts and chat rate limits. */
int net_poll(net *n, double now, net_event *ev);
/* Sends queued packets now; call once a frame after sending. */
void net_flush(net *n);

/* A game message to a player (host) or to the host (player). Returns 0
 * on success, -1 if there is no such player or link. */
int net_send(net *n, int player, int ch, const uint8_t *data, size_t len);
/* The host's message to every player. */
void net_broadcast(net *n, int ch, const uint8_t *data, size_t len);
/* Chat to every other player, end-to-end. Returns how many it was sent
 * to (players whose pair keys are not ready yet get it once they are). */
int net_chat(net *n, const char *text);

int net_is_host(const net *n);
int net_me(const net *n);                  /* this player's id, -1 until joined */
int net_player_present(const net *n, int player);
const char *net_player_name(const net *n, int player); /* "" if absent */
/* The identity key the host announced for this player (the host's own
 * from the handshake), or NULL if absent. */
const uint8_t *net_player_key(const net *n, int player);
/* 1 if chat with this player goes over a direct link, 0 if through the
 * host (or not at all yet). */
int net_player_direct(const net *n, int player);
/* The host's identity key once the link to it is up (for pinning), or
 * NULL; on the host, its own. */
const uint8_t *net_host_key(const net *n);
/* The UDP port this side listens on. */
uint16_t net_port(const net *n);

/* For the tests: never dial other players, so chat goes through the host. */
void net_test_no_mesh(net *n);
/* For fuzzing: a host with no socket, whose players are added by hand
 * (each arrives as NET_EV_JOINED). Messages to them go nowhere. */
net *net_offline_host(const nc_keypair *id, const char *name);
void net_test_add_player(net *n, int player, const char *name, const uint8_t *pk);

#endif
