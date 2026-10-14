/* Multiplayer game state on top of net.c. The host is the authority: it
 * simulates the world and every player's body, health, inventory and
 * hands from the intents the others send, and saves the world. A client
 * sends intents, predicts its own movement and corrects it against the
 * host's snapshots; everything else it shows comes from the host.
 *
 * Client to host: inputs every physics step on the unreliable channel
 * (each packet repeats the last few, and clicks are running counters, so
 * a lost packet loses nothing), inventory operations, treatments and
 * respawning on the reliable one, and requests for terrain columns.
 * Host to client: columns in the save format, block edits, snapshots
 * (the client's own movement with the last input applied, the other
 * players, falling blocks and items nearby, the time of day) twenty
 * times a second, the body ten times a second, the inventory when it
 * changes, and short messages.
 *
 * Everything from the network is untrusted, from either side, and goes
 * through checked decoders. What a client asks for is bounded too:
 * queued inputs, column requests per frame, slot and recipe indices.
 *
 * Not here: window, input devices, drawing (main.c), sockets and crypto
 * (net.c). Known limits: the host's world streams around the host, so
 * players far from it stand still at the edge of what it has loaded;
 * joined players' health is not saved (nor is the host's). */
#ifndef MC_MP_H
#define MC_MP_H

#include "combat.h"
#include "health.h"
#include "interact.h"
#include "inventory.h"
#include "net.h"
#include "physics.h"
#include "thermo.h"
#include "world.h"

#define MP_CHAT_LINES 8

struct save_db;

/* The game state mp works on; not owned. On a client, pl, hl and inv are
 * the local player's, which mp corrects from the host's messages. */
typedef struct {
    world *w;
    physics *ph;
    thermo *th;
    fx_state *fx;
    player *pl;
    health *hl;
    inventory *inv;
    uint32_t seed;
    /* netkeys file pinning each player's key by name, or NULL. The host
     * vouches for every key, so this is how a player notices a host that
     * swaps one (to read chat in the middle). */
    const char *known_players;
    /* The creatures and bullets the host simulates for everyone (NULL on a
     * client, which draws the mobs it is sent). */
    mobs *mobs;
    combat *cb;
    /* The host's world database, where joined players are kept by
     * identity key (NULL: not saved, and a client). */
    struct save_db *db;
} mp_world;

/* Inventory operations a client sends; the host applies the same call. */
typedef enum {
    MP_OP_CLICK,          /* inv_click */
    MP_OP_CRAFT,          /* inv_craft, once or as often as it goes */
    MP_OP_DROP_CURSOR,    /* the stack on the cursor thrown out of the window */
    MP_OP_RETURN_CURSOR,  /* the inventory closed: the cursor stack goes back */
    MP_OP_TREAT,          /* health_treat */
    MP_OP_RESPAWN,
    MP_OP_COUNT
} mp_op_kind;
typedef struct {
    uint8_t kind, a, b, c; /* CLICK: slot, button, shift. CRAFT: recipe, all. TREAT: part, treatment. */
} mp_op;

/* Another player as the game draws them. */
typedef struct {
    const char *name;
    dvec3 pos;      /* feet, interpolated */
    int id;
    float yaw, pitch;
    int flying, sneaking, dead;
} mp_avatar;

typedef struct {
    char who[NET_NAME_MAX + 1]; /* "" for a notice from the game */
    char text[NET_CHAT_MAX + 1];
    double at;                  /* when it arrived, for fading */
} mp_chat_line;

typedef struct mp mp;

/* Takes ownership of n (NULL: a client that is never connected, for
 * fuzzing what a host may send). The host's world must be ready; a client's is
 * set up once the host's welcome says the seed (mp_welcome), then
 * handed over with mp_attach. */
mp *mp_create(net *n, const mp_world *ctx);
/* Client: 1 once the host has welcomed this player, with the world seed. */
int mp_welcome(const mp *m, uint32_t *seed);
/* Client: the world is set up (from the welcome's seed); ctx replaces the
 * one given at creation, and columns are requested from the host from now
 * on. */
void mp_attach(mp *m, const mp_world *ctx);
void mp_destroy(mp *m);
int mp_is_host(const mp *m);

/* Network events: joins and leaves, chat, and on a client everything the
 * host sends. Call once a frame before simulating. */
void mp_poll(mp *m, double now);
/* A client is in once the host has welcomed it and sent the first
 * snapshot; until then it has nothing to show. */
int mp_ready(const mp *m);
/* The session ended (host left, refused, lost); *why says why. */
int mp_ended(const mp *m, const char **why);

/* Host: one fixed step for every player. Moves the local player with
 * local_in (index 0 of the physics' contacts) and each remote player
 * with its next input, then runs the remote players' bodies and hands.
 * The caller handles its own player's body as in single player. */
void mp_host_step(mp *m, const player_input *local_in);
/* The bodies the host's last step could hit, for the host player's own
 * blows (index 0 is the host's player). */
combat_targets mp_targets(mp *m);
/* On a client, the creatures near this player as the host last sent them
 * (positions, headings and kinds only, for drawing). */
const mobs *mp_client_mobs(const mp *m);
/* Host: writes every joined player, present or gone this session, into
 * the world database as "player:<key hex>" (the single-player record:
 * position, view, inventory), for the next time that key joins. Call
 * inside the transaction that saves the world. A dead player keeps the
 * record saved before. 0, or -1 if a write failed. */
int mp_save_players(const mp *m);
/* Host: a block changed (the world's edit hook): sent at the next flush. */
void mp_on_block_changed(mp *m, int x, int y, int z);

/* Client: one fixed step of prediction. in is the movement input before
 * survival limits (both sides apply them, each from its copy of the
 * body), attack whether the break button is held. */
void mp_client_step(mp *m, const player_input *in, int attack);
/* Client: a click this frame, counted into the next inputs. */
typedef enum { MP_CLICK_BREAK, MP_CLICK_PLACE, MP_CLICK_PICK, MP_CLICK_DROP, MP_CLICK_DROP_ALL } mp_click;
void mp_client_click(mp *m, mp_click c);
/* Client: an operation, applied to the local copy by the caller first. */
void mp_client_op(mp *m, mp_op op);
/* Client: the block the host says this player is breaking. */
int mp_client_crack(const mp *m, ipos *at, float *progress);

/* Sends what is due (inputs, snapshots, edits, the body, inventories) and
 * flushes the network. Call once a frame after simulating. */
void mp_flush(mp *m, double now);

/* Chat: sends a line to everyone (end-to-end) and shows it here. A line
 * starting with '/' is a command for this side and is not sent:
 * "/safety [name]" shows the safety number with that player, or with
 * everyone. */
void mp_say(mp *m, const char *text);
/* The newest lines, oldest first; returns how many. */
int mp_chat(const mp *m, const mp_chat_line **lines);
/* A message for the HUD from the host (a treatment's result), once. */
const char *mp_take_message(mp *m);

/* The other players, for drawing. alpha is the physics interpolation
 * factor (host), now the clock (client, which draws slightly in the past
 * to interpolate between snapshots). */
int mp_avatars(const mp *m, double now, double alpha, mp_avatar *out, int max);

/* For the tests and fuzzing: one message as if it arrived from player
 * `from` on channel ch. */
void mp_receive(mp *m, int from, int ch, const uint8_t *data, size_t len);

#endif
