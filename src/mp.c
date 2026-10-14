#include "mp.h"
#include "log.h"
#include "mem.h"
#include "netkeys.h"
#include "save.h"
#include "save_db.h"
#include "survival.h"
#include "wire.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_REDUNDANCY 4 /* inputs repeated per packet: three lost in a row cost nothing */
#define INPUT_QUEUE 16
#define INPUT_KEEP 8       /* more queued than this and the oldest go: latency stays bounded */
#define HISTORY 256        /* client inputs kept for replaying, ~4 s */
#define SNAP_EVERY 3       /* physics steps between snapshots: 20 Hz */
#define HEALTH_EVERY 6     /* 10 Hz */
#define COLUMN_QUEUE 256
#define COLUMNS_PER_FRAME 8 /* per player: at worst ~3 ms of generation a frame */
#define COLUMN_REACH 40     /* columns from the player a request may ask for */
#define EDITS_MAX 4096
#define EDIT_SET 8192       /* power of two, twice EDITS_MAX */
#define VIEW_ENTS 64        /* falling blocks, and items, per snapshot */
#define VIEW_RANGE 48.0
#define INTERP_DELAY 0.1    /* s a client draws others in the past */
#define CLICKS 5
#define GONE_MAX 16          /* departed players remembered for a rejoin */
#define COLUMN_RATE 64.0     /* columns per second a player may be sent ... */
#define COLUMN_BURST 160.0   /* ... after a burst of this many (joining at radius 8) */
#define COLUMN_RETRY 3.0     /* s between a client's passes over columns the host has not sent */
_Static_assert(SAVE_MAX_FILE + 9 <= NET_MAX_MESSAGE, "a column message must fit ENet's packet cap");

enum { C_INPUT = 1, C_OP, C_COLUMN };
enum { H_WELCOME = 16, H_COLUMN, H_EDITS, H_SNAP, H_HEALTH, H_INV, H_MSG };
enum { B_JUMP = 1, B_DESCEND = 2, B_SPRINT = 4, B_SNEAK = 8, B_ATTACK = 16, B_FLY = 32 };
enum { F_FLYING = 1, F_SNEAKING = 2, F_DEAD = 4 };

typedef struct {
    float forward, right;
    uint8_t buttons;
    float yaw, pitch;
    uint8_t selected;
    uint8_t clicks[CLICKS]; /* running counters, one per mp_click */
} intent;

typedef struct {
    int active;
    player pl;
    health *hl;
    inventory inv;
    interact ia;
    intent q[INPUT_QUEUE];
    uint32_t q_seq[INPUT_QUEUE];
    int qh, qn;
    uint32_t newest;  /* newest input sequence number queued */
    uint32_t applied; /* the one last applied */
    intent cur;       /* held while no new input arrives */
    uint8_t done[CLICKS];
    int actions;
    uint32_t rng;
    uint32_t ops_done, ops_told; /* operations applied, and as last told to the client */
    uint8_t inv_sent[INV_ENCODED_SIZE];
    int inv_sent_valid;
    int col[COLUMN_QUEUE][2];
    int col_h, col_n;
    char msg[96];
    ipos crack;
    float crack_p;
    int cracking;
    double col_tokens, col_t; /* column send budget */
} remote;

/* A player who left, by identity key: rejoining gives them back what they
 * had instead of a fresh kit, so leaving and joining again makes nothing. */
typedef struct {
    int used;
    uint32_t age;
    uint8_t pk[NC_KEY_BYTES];
    health *hl;
    inventory inv;
    player pl;
    uint32_t rng;
} gone;

typedef struct {
    int present;
    dvec3 pos[2];
    float yaw[2], pitch[2];
    double t[2];
    uint8_t flags;
} ghost;

typedef struct {
    int welcomed, have_snap, attached;
    uint32_t seed;
    uint32_t seq;
    intent hist[HISTORY];
    player_input raw[HISTORY];
    uint8_t clicks[CLICKS];
    uint32_t ops_sent;
    ghost g[NET_MAX_PLAYERS];
    ipos crack;
    float crack_p;
    int cracking;
    char msg[96], taken[96];
    mobs mobs;           /* the creatures near this player, as last sent */
    double retry_t;      /* last world_retry_requests */
} client;

struct mp {
    net *n;
    mp_world ctx;
    int host, ended, dedicated;
    char why[NET_CHAT_MAX + 1];
    remote r[NET_MAX_PLAYERS]; /* host: by player id, 0 unused */
    client c;
    mp_chat_line chat[MP_CHAT_LINES];
    int chat_n;
    uint32_t chat_total;
    char names[NET_MAX_PLAYERS][NET_NAME_MAX + 1]; /* kept past a player leaving, for the notice */
    uint8_t keys[NET_MAX_PLAYERS][NC_KEY_BYTES];   /* ... and their keys, to remember what they had */
    ipos edits[EDITS_MAX];
    int edit_n;
    uint16_t edit_set[EDIT_SET]; /* index + 1 into edits, 0 empty */
    gone gone[GONE_MAX];
    uint32_t gone_age;
    uint32_t step;
    double now;
    /* This step's bodies that blows, bites and bullets can reach: index 0
     * is the host's player, then the remotes in the order stepped. */
    player *tpl[PHYS_MAX_PLAYERS];
    health *thl[PHYS_MAX_PLAYERS];
    int tn;
    uint8_t *buf;
    size_t buf_cap;
};

/* ---------------------------------------------------------------- helpers */

static void chat_add(mp *m, const char *who, const char *text)
{
    if (m->chat_n == MP_CHAT_LINES) {
        memmove(m->chat, m->chat + 1, sizeof m->chat[0] * (MP_CHAT_LINES - 1));
        m->chat_n--;
    }
    m->chat_total++;
    mp_chat_line *l = &m->chat[m->chat_n++];
    snprintf(l->who, sizeof l->who, "%s", who);
    snprintf(l->text, sizeof l->text, "%s", text);
    l->at = m->now;
}

static void put_intent(wire_w *w, const intent *in)
{
    wire_u8(w, (uint8_t)(int8_t)lrintf(in->forward * 127.0f));
    wire_u8(w, (uint8_t)(int8_t)lrintf(in->right * 127.0f));
    wire_u8(w, in->buttons);
    wire_f32(w, in->yaw);
    wire_f32(w, in->pitch);
    wire_u8(w, in->selected);
    wire_bytes(w, in->clicks, CLICKS);
}

static void get_intent(wire_r *r, intent *in)
{
    in->forward = fmaxf(-1.0f, (float)(int8_t)wire_get_u8(r) / 127.0f);
    in->right = fmaxf(-1.0f, (float)(int8_t)wire_get_u8(r) / 127.0f);
    in->buttons = wire_get_u8(r);
    in->yaw = wire_get_f32(r);
    in->pitch = wire_get_f32(r);
    in->selected = wire_get_u8(r);
    wire_get_bytes(r, in->clicks, CLICKS);
    if (in->selected >= INV_HOTBAR || fabsf(in->yaw) > 1000.0f) r->bad = 1;
    in->yaw = fmodf(in->yaw, (float)(2 * MC_PI));
    in->pitch = fminf(fmaxf(in->pitch, -1.55f), 1.55f);
}

static player_input movement(const intent *in)
{
    player_input p = {0};
    p.forward = in->forward;
    p.right = in->right;
    p.jump = (in->buttons & B_JUMP) != 0;
    p.descend = (in->buttons & B_DESCEND) != 0;
    p.sprint = (in->buttons & B_SPRINT) != 0;
    p.sneak = (in->buttons & B_SNEAK) != 0;
    return p;
}

/* Fly mode follows the intent, as the F key does in single player. */
static void set_flying(player *p, int on)
{
    if (!p->flying == !on) return;
    p->flying = on;
    p->vel = dv3(0, 0, 0);
}

static dvec3 lerp3(dvec3 a, dvec3 b, double f)
{
    dvec3 d = dv3(b.x - a.x, b.y - a.y, b.z - a.z);
    return dv3(a.x + d.x * f, a.y + d.y * f, a.z + d.z * f);
}

static uint8_t *scratch(mp *m, size_t need)
{
    if (need > m->buf_cap) {
        m->buf = mem_realloc(m->buf, need);
        m->buf_cap = need;
    }
    return m->buf;
}

/* ---------------------------------------------------------------- setup */

mp *mp_create(net *n, const mp_world *ctx)
{
    mp *m = mem_calloc(1, sizeof *m);
    m->n = n;
    m->ctx = *ctx;
    m->host = n && net_is_host(n);
    scratch(m, SAVE_MAX_FILE + 64);
    return m;
}

static void column_request(void *user, int cx, int cz);

void mp_attach(mp *m, const mp_world *ctx)
{
    if (m->host || m->c.attached) return;
    m->ctx = *ctx;
    m->ctx.w->remote_user = m;
    m->ctx.w->on_column_request = column_request;
    m->c.attached = 1;
}

void mp_destroy(mp *m)
{
    if (!m) return;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) mem_free(m->r[i].hl);
    for (int i = 0; i < GONE_MAX; i++) mem_free(m->gone[i].hl);
    if (m->c.attached) m->ctx.w->on_column_request = NULL;
    net_destroy(m->n);
    mem_free(m->buf);
    mem_free(m);
}

int mp_is_host(const mp *m) { return m->host; }

void mp_set_dedicated(mp *m) { m->dedicated = m->host; }

int mp_kick(mp *m, const char *name)
{
    for (int id = 1; m->host && id < NET_MAX_PLAYERS; id++)
        if (net_player_present(m->n, id) && strcmp(net_player_name(m->n, id), name) == 0) return net_kick(m->n, id);
    return -1;
}

int mp_welcome(const mp *m, uint32_t *seed)
{
    if (m->host || !m->c.welcomed) return 0;
    *seed = m->c.seed;
    return 1;
}

int mp_ready(const mp *m) { return m->host || (m->c.attached && m->c.have_snap); }

int mp_ended(const mp *m, const char **why)
{
    if (m->ended && why) *why = m->why;
    return m->ended;
}

/* ---------------------------------------------------------------- host */

/* The database row of the player with this identity key. */
static void saved_name(const uint8_t *key, char *out, size_t cap)
{
    char hex[2 * NC_KEY_BYTES + 1] = {0};
    nc_key_hex(key, hex);
    snprintf(out, cap, "player:%s", hex);
}

/* What the world kept of this key from an earlier session: 1 with *pl and
 * *inv set, 0 if nothing usable (the row is untrusted and decoded with
 * the single-player checks). */
static int load_saved(const mp *m, const uint8_t *key, player *pl, inventory *inv)
{
    if (!m->ctx.db || !key) return 0;
    char name[80] = {0};
    saved_name(key, name, sizeof name);
    uint8_t buf[PLAYER_FILE_SIZE + 1] = {0};
    long n = save_db_get_blob(m->ctx.db, name, buf, sizeof buf);
    if (n <= 0) return 0;
    if (survival_decode_player(pl, inv, NULL, buf, (size_t)n) != 0) {
        log_warn("mp: the saved state of %.15s... is corrupt; a fresh start", name);
        return 0;
    }
    return 1;
}

static int save_one(const mp *m, const uint8_t *key, const player *pl, const inventory *inv)
{
    char name[80] = {0};
    saved_name(key, name, sizeof name);
    uint8_t buf[PLAYER_FILE_SIZE] = {0};
    size_t n = survival_encode_player(pl, inv, thermo_day_time(m->ctx.th), buf, sizeof buf);
    return n && save_db_put_blob(m->ctx.db, name, buf, n) == 0 ? 0 : -1;
}

int mp_save_players(const mp *m)
{
    if (!m->host || !m->ctx.db) return 0;
    for (int id = 1; id < NET_MAX_PLAYERS; id++) {
        const remote *r = &m->r[id];
        if (r->active && !r->hl->dead && save_one(m, m->keys[id], &r->pl, &r->inv) != 0) return -1;
    }
    for (int i = 0; i < GONE_MAX; i++) {
        const gone *g = &m->gone[i];
        if (g->used && !(g->hl && g->hl->dead) && save_one(m, g->pk, &g->pl, &g->inv) != 0) return -1;
    }
    return 0;
}

static void host_join(mp *m, int id)
{
    remote *r = &m->r[id];
    mem_free(r->hl);
    memset(r, 0, sizeof *r);
    r->active = 1;
    r->col_tokens = COLUMN_BURST;
    r->col_t = m->now;
    const uint8_t *key = net_player_key(m->n, id);
    gone *back = NULL;
    player was = {0};
    int have_was = 0;
    for (int i = 0; i < GONE_MAX && key; i++)
        if (m->gone[i].used && memcmp(m->gone[i].pk, key, NC_KEY_BYTES) == 0) back = &m->gone[i];
    if (back) {
        r->hl = back->hl;
        r->inv = back->inv;
        r->rng = back->rng;
        was = back->pl;
        have_was = 1;
        back->hl = NULL;
        back->used = 0;
    } else {
        r->rng = m->ctx.seed ^ (uint32_t)id * 2654435761u ^
                 (key ? (uint32_t)key[0] << 24 | (uint32_t)key[1] << 16 : 0u);
        r->hl = mem_alloc(sizeof *r->hl);
        health_init_varied(r->hl, r->rng);
        inv_init(&r->inv);
        have_was = load_saved(m, key, &was, &r->inv);
        if (!have_was) inv_starting_kit(&r->inv);
    }
    interact_init(&r->ia, r->rng);
    /* Where they were (they wait there while the terrain loads), else
     * next to the host. */
    const player *h = m->ctx.pl;
    if (have_was) {
        r->pl = was;
        r->pl.vel = dv3(0, 0, 0);
        r->pl.prev_pos = r->pl.pos;
    } else {
        player_spawn(&r->pl, m->ctx.w, floor(h->pos.x) + 0.5 + (double)(id % 3) - 1.0, floor(h->pos.z) + 3.5);
    }
    r->cur.selected = 0;

    uint8_t buf[32] = {0};
    wire_w w = wire_writer(buf, sizeof buf);
    wire_u8(&w, H_WELCOME);
    wire_u32(&w, m->ctx.seed);
    wire_f64(&w, thermo_day_time(m->ctx.th));
    net_send(m->n, id, NET_CH_GAME, buf, w.len);
}

/* Keeps what a leaving player had, by key, for the rest of the session. */
static void host_leave(mp *m, int id, const uint8_t *key)
{
    remote *r = &m->r[id];
    if (!r->active) return;
    if (key) {
        gone *g = &m->gone[0];
        for (int i = 0; i < GONE_MAX; i++) {
            if (m->gone[i].used && memcmp(m->gone[i].pk, key, NC_KEY_BYTES) == 0) {
                g = &m->gone[i];
                break;
            }
            if (!m->gone[i].used || m->gone[i].age < g->age) g = &m->gone[i];
        }
        mem_free(g->hl);
        g->used = 1;
        g->age = ++m->gone_age;
        memcpy(g->pk, key, NC_KEY_BYTES);
        g->hl = r->hl;
        g->inv = r->inv;
        g->pl = r->pl;
        g->rng = r->rng;
        r->hl = NULL;
    }
    mem_free(r->hl);
    memset(r, 0, sizeof *r);
}

static void host_input(remote *r, wire_r *rd)
{
    uint32_t newest = wire_get_u32(rd);
    int count = wire_get_u8(rd);
    intent in[INPUT_REDUNDANCY] = {{0}};
    if (count < 1 || count > INPUT_REDUNDANCY) rd->bad = 1;
    for (int i = 0; i < count && !rd->bad; i++) get_intent(rd, &in[i]);
    if (rd->bad || rd->pos != rd->len) return;
    for (int i = count - 1; i >= 0; i--) {
        uint32_t s = newest - (uint32_t)i;
        if ((int32_t)(s - r->newest) <= 0) continue; /* already have it */
        if (r->qn == INPUT_QUEUE) {
            r->qh = (r->qh + 1) % INPUT_QUEUE;
            r->qn--;
        }
        int at = (r->qh + r->qn++) % INPUT_QUEUE;
        r->q[at] = in[i];
        r->q_seq[at] = s;
        r->newest = s;
    }
}

static void host_treat(const mp *m, remote *r, int part, int what)
{
    if (r->hl->dead) return;
    vec3 look = look_dir(r->pl.yaw, r->pl.pitch);
    int water = survival_water_nearby(m->ctx.w, &r->pl, look) || inv_held(&r->inv)->id == I_WATER_BUCKET;
    health_treat(r->hl, &r->inv, part, what, water, r->msg, sizeof r->msg);
}

static void host_op(mp *m, remote *r, wire_r *rd)
{
    int kind = wire_get_u8(rd), a = wire_get_u8(rd), b = wire_get_u8(rd), c = wire_get_u8(rd);
    if (rd->bad || rd->pos != rd->len || kind >= MP_OP_COUNT) return;
    r->ops_done++;
    switch (kind) {
    case MP_OP_CLICK:
        if (a < INV_SLOTS && b <= 1 && c <= 1) inv_click(&r->inv, a, b, c);
        break;
    case MP_OP_CRAFT:
        if (a < g_recipe_count) {
            int near = thermo_stations(m->ctx.th, m->ctx.w, r->pl.pos), n = 0;
            while (inv_craft(&r->inv, a, near) && ++n < (b ? 64 : 1)) {}
        }
        break;
    case MP_OP_DROP_CURSOR: {
        item_stack st = r->inv.cursor;
        r->inv.cursor = (item_stack){0, 0, 0};
        survival_toss(m->ctx.ph, &r->inv, &r->pl, st);
    } break;
    case MP_OP_RETURN_CURSOR: survival_toss(m->ctx.ph, &r->inv, &r->pl, inv_return_cursor(&r->inv)); break;
    case MP_OP_TREAT:
        if (a < BP_COUNT && b < TREAT_COUNT) host_treat(m, r, a, b);
        break;
    case MP_OP_RESPAWN:
        if (r->hl->dead) {
            survival_drop_all(m->ctx.ph, &r->inv, &r->pl, &r->rng);
            const player *h = m->ctx.pl;
            player_spawn(&r->pl, m->ctx.w, floor(h->pos.x) + 1.5, floor(h->pos.z) + 0.5);
            r->rng = r->rng * 1664525u + 1013904223u;
            health_init_varied(r->hl, r->rng);
            inv_starting_kit(&r->inv);
        }
        break;
    default: break;
    }
}

static void host_column(remote *r, wire_r *rd)
{
    int cx = wire_get_i32(rd), cz = wire_get_i32(rd);
    if (rd->bad || rd->pos != rd->len || r->col_n == COLUMN_QUEUE) return;
    int pcx = chunk_of((int)floor(r->pl.pos.x)), pcz = chunk_of((int)floor(r->pl.pos.z));
    if (llabs((long long)cx - pcx) > COLUMN_REACH || llabs((long long)cz - pcz) > COLUMN_REACH) return;
    /* A client asks again for what it has waited long for: once is enough. */
    for (int k = 0; k < r->col_n; k++) {
        const int *q = r->col[(r->col_h + k) % COLUMN_QUEUE];
        if (q[0] == cx && q[1] == cz) return;
    }
    int at = (r->col_h + r->col_n++) % COLUMN_QUEUE;
    r->col[at][0] = cx;
    r->col[at][1] = cz;
}

void mp_on_block_changed(mp *m, int x, int y, int z)
{
    if (!m->host) return;
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u;
    for (uint32_t i = 0;; i++) {
        uint16_t *slot = &m->edit_set[(h + i) & (EDIT_SET - 1)];
        if (!*slot) {
            if (m->edit_n == EDITS_MAX) return; /* flushed every frame; a burst past this is dropped */
            m->edits[m->edit_n] = (ipos){x, y, z};
            *slot = (uint16_t)++m->edit_n;
            return;
        }
        const ipos *e = &m->edits[*slot - 1];
        if (e->x == x && e->y == y && e->z == z) return;
    }
}

/* The other players, falling blocks and items near r, and r's own state. */
static void host_snapshot(mp *m, int id)
{
    remote *r = &m->r[id];
    const physics *ph = m->ctx.ph;
    uint8_t *buf = scratch(m, 16384);
    wire_w w = wire_writer(buf, 16384);
    wire_u8(&w, H_SNAP);
    wire_u32(&w, r->applied);
    wire_f64(&w, thermo_day_time(m->ctx.th));
    uint8_t pb[PLAYER_NET_BYTES] = {0};
    size_t pn = player_encode(&r->pl, pb, sizeof pb);
    wire_u8(&w, (uint8_t)pn);
    wire_bytes(&w, pb, pn);
    wire_u8(&w, (uint8_t)r->cracking);
    wire_i32(&w, r->crack.x);
    wire_u8(&w, (uint8_t)(r->crack.y < 0 ? 0 : r->crack.y > 255 ? 255 : r->crack.y));
    wire_i32(&w, r->crack.z);
    wire_f32(&w, r->crack_p);

    int others = 0;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) others += i != id && (i ? m->r[i].active : !m->dedicated);
    wire_u8(&w, (uint8_t)others);
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (i == id || !(i ? m->r[i].active : !m->dedicated)) continue;
        const player *p = i ? &m->r[i].pl : m->ctx.pl;
        int dead = i ? m->r[i].hl->dead : m->ctx.hl->dead;
        wire_u8(&w, (uint8_t)i);
        wire_f64(&w, p->pos.x);
        wire_f64(&w, p->pos.y);
        wire_f64(&w, p->pos.z);
        wire_f32(&w, p->yaw);
        wire_f32(&w, p->pitch);
        wire_u8(&w, (uint8_t)((p->flying ? F_FLYING : 0) | (p->sneaking ? F_SNEAKING : 0) | (dead ? F_DEAD : 0)));
    }

    dvec3 me = r->pl.pos;
    size_t at = w.len;
    int n = 0;
    wire_u8(&w, 0);
    for (int i = 0; i < ph->body_count && n < VIEW_ENTS; i++) {
        const body *b = &ph->bodies[i];
        if (fabs(b->pos.x - me.x) > VIEW_RANGE || fabs(b->pos.z - me.z) > VIEW_RANGE) continue;
        wire_f64(&w, b->pos.x);
        wire_f64(&w, b->pos.y);
        wire_f64(&w, b->pos.z);
        for (int k = 0; k < 4; k++) wire_f32(&w, b->rot[k]);
        wire_u8(&w, b->block);
        n++;
    }
    if (!w.bad) buf[at] = (uint8_t)n;
    at = w.len;
    n = 0;
    wire_u8(&w, 0);
    for (int i = 0; i < ph->item_count && n < VIEW_ENTS; i++) {
        const item_ent *it = &ph->items[i];
        if (fabs(it->pos.x - me.x) > VIEW_RANGE || fabs(it->pos.z - me.z) > VIEW_RANGE) continue;
        wire_f64(&w, it->pos.x);
        wire_f64(&w, it->pos.y);
        wire_f64(&w, it->pos.z);
        wire_f32(&w, it->spin);
        wire_u8(&w, it->id);
        wire_u8(&w, it->count);
        n++;
    }
    if (!w.bad) buf[at] = (uint8_t)n;
    /* Creatures near this player, for drawing; the host keeps the rest. */
    at = w.len;
    n = 0;
    wire_u8(&w, 0);
    for (int i = 0; m->ctx.mobs && i < m->ctx.mobs->count && n < MOB_MAX; i++) {
        const mob *mb = &m->ctx.mobs->m[i];
        if (fabs(mb->body.pos.x - me.x) > VIEW_RANGE || fabs(mb->body.pos.z - me.z) > VIEW_RANGE) continue;
        wire_f64(&w, mb->body.pos.x);
        wire_f64(&w, mb->body.pos.y);
        wire_f64(&w, mb->body.pos.z);
        wire_f32(&w, mb->body.yaw);
        wire_f32(&w, mb->stride);
        wire_u8(&w, mb->kind);
        wire_u8(&w, (uint8_t)(mb->hurt_t > 0.0f));
        n++;
    }
    if (!w.bad) {
        buf[at] = (uint8_t)n;
        net_send(m->n, id, NET_CH_STATE, buf, w.len);
    }
}

/* A remote player's hands for one step, from its intent: as frame_interact
 * does for the local player, at the physics rate. */
static void host_hands(mp *m, remote *r, int who)
{
    const intent *in = &r->cur;
    int click[CLICKS] = {0};
    for (int i = 0; i < CLICKS; i++) {
        /* One click of each kind per step; more wait for the next. */
        click[i] = (uint8_t)(in->clicks[i] - r->done[i]) != 0;
        r->done[i] = (uint8_t)(r->done[i] + click[i]);
    }
    player *pl = &r->pl;
    health_limits lim = health_get_limits(r->hl);
    dvec3 eye = dv3(pl->pos.x, pl->pos.y + PLAYER_EYE - (pl->sneaking ? 0.3 : 0.0), pl->pos.z);
    vec3 look = look_dir(pl->yaw, pl->pitch);
    ray_hit hit = physics_raycast(m->ctx.w, eye, look, 5.0);
    interact_input ii = {.attack = (in->buttons & B_ATTACK) != 0,
                         .attack_click = click[MP_CLICK_BREAK],
                         .use_click = click[MP_CLICK_PLACE],
                         .pick_click = click[MP_CLICK_PICK],
                         .drop = click[MP_CLICK_DROP_ALL] ? 2 : click[MP_CLICK_DROP],
                         .can_act = pl->flying || (lim.has_control && lim.can_act),
                         .speed = pl->flying ? 1.0f : 0.4f + 0.6f * lim.move_scale,
                         .dt = (float)PHYS_DT};
    if (m->ctx.mobs && m->ctx.cb) {
        /* A creature or player in reach takes the blow instead of the block
         * behind it. */
        combat_targets t = {m->tpl, m->thl, m->tn};
        double block_d = hit.hit ? dv3_len(dv3(hit.block.x + 0.5 - eye.x, hit.block.y + 0.5 - eye.y,
                                               hit.block.z + 0.5 - eye.z))
                                 : 5.0;
        if (combat_aim(m->ctx.mobs, &t, who, eye, look, block_d)) ii.attack = 0;
        if (ii.can_act) {
            combat_out co = combat_frame(m->ctx.cb, m->ctx.mobs, m->ctx.ph, &t, who, &r->inv, eye, look, block_d,
                                         ii.attack_click, ii.use_click, (float)PHYS_DT);
            if (co.msg) snprintf(r->msg, sizeof r->msg, "%s", co.msg);
            if (co.struck || co.fired) ii.attack_click = 0;
        }
    }
    interact_out io = interact_frame(&r->ia, m->ctx.w, m->ctx.ph, m->ctx.th, pl, &r->inv, m->ctx.fx, eye, look, hit,
                                     &ii);
    r->actions += io.actions;
    if (io.broke) survival_on_break(r->hl, io.broken_id);
    if (io.eat) host_treat(m, r, BP_ABDOMEN, TREAT_EAT);
    if (io.defib) host_treat(m, r, BP_CHEST, TREAT_DEFIB);
    if (io.msg) snprintf(r->msg, sizeof r->msg, "%s", io.msg);
    r->cracking = r->ia.has_target && !pl->flying;
    r->crack = r->ia.target;
    r->crack_p = r->ia.progress;
    if (r->hl->defib.phase != DEFIB_OFF && !inv_count(&r->inv, I_DEFIBRILLATOR) && r->inv.cursor.id != I_DEFIBRILLATOR)
        health_pads(r->hl, 0);
}

const mobs *mp_client_mobs(const mp *m) { return &m->c.mobs; }

int mp_anchors(const mp *m, int (*at)[2], int max)
{
    int n = 0;
    for (int id = 1; m->host && id < NET_MAX_PLAYERS && n < max; id++) {
        if (!m->r[id].active) continue;
        at[n][0] = chunk_of((int)floor(m->r[id].pl.pos.x));
        at[n][1] = chunk_of((int)floor(m->r[id].pl.pos.z));
        n++;
    }
    return n;
}

/* The terrain a player could touch this step is loaded: their column and
 * its neighbours. Until then they wait where they are (unloaded cells
 * count as solid, and the body would be stuck inside one). */
static int terrain_ready(const world *w, const player *p)
{
    int cx = chunk_of((int)floor(p->pos.x)), cz = chunk_of((int)floor(p->pos.z));
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++)
            if (!world_column(w, cx + dx, cz + dz)) return 0;
    return 1;
}

combat_targets mp_targets(mp *m) { return (combat_targets){m->tpl, m->thl, m->tn}; }

void mp_host_step(mp *m, const player_input *local_in)
{
    player *ps[PHYS_MAX_PLAYERS] = {m->ctx.pl};
    player_input ins[PHYS_MAX_PLAYERS] = {*local_in};
    int who[PHYS_MAX_PLAYERS] = {0};
    survival_before sb[PHYS_MAX_PLAYERS] = {0};
    int n = 1;
    for (int id = 1; id < NET_MAX_PLAYERS && n < PHYS_MAX_PLAYERS; id++) {
        remote *r = &m->r[id];
        if (!r->active || !terrain_ready(m->ctx.w, &r->pl)) continue;
        while (r->qn > INPUT_KEEP) {
            r->qh = (r->qh + 1) % INPUT_QUEUE;
            r->qn--;
        }
        if (r->qn) {
            r->cur = r->q[r->qh];
            r->applied = r->q_seq[r->qh];
            r->qh = (r->qh + 1) % INPUT_QUEUE;
            r->qn--;
        }
        r->pl.yaw = r->cur.yaw;
        r->pl.pitch = r->cur.pitch;
        r->inv.selected = r->cur.selected;
        set_flying(&r->pl, (r->cur.buttons & B_FLY) != 0);
        player_input in = {0};
        if (!r->hl->dead) in = movement(&r->cur);
        survival_limit_input(r->hl, &r->pl, &in);
        m->ctx.ph->contact[n].magnet = !r->hl->dead;
        sb[n] = survival_capture(&r->pl);
        ps[n] = &r->pl;
        ins[n] = in;
        who[n++] = id;
    }
    physics_step_players(m->ctx.ph, ps, ins, n);
    m->tn = n;
    for (int k = 0; k < n; k++) {
        health *h = k ? m->r[who[k]].hl : m->ctx.hl;
        int out = h->dead || (k == 0 && m->dedicated); /* a server's stand-in is never a target */
        m->tpl[k] = out ? NULL : ps[k];
        m->thl[k] = out ? NULL : h;
    }
    if (m->ctx.mobs && m->ctx.cb) {
        combat_targets t = {m->tpl, m->thl, m->tn};
        combat_creatures(m->ctx.cb, m->ctx.mobs, m->ctx.ph, &t, thermo_daylight(m->ctx.th));
    }
    for (int k = 1; k < n; k++) {
        remote *r = &m->r[who[k]];
        health_env env = {0};
        survival_env(m->ctx.w, &r->pl, &sb[k], r->actions, &env);
        survival_thermal(m->ctx.th, m->ctx.w, &r->pl, &env);
        r->actions = 0;
        survival_impacts(r->hl, m->ctx.w, &m->ctx.ph->contact[k], &r->pl, &sb[k]);
        health_step(r->hl, &env, PHYS_DT);
        physics_pickup(m->ctx.ph, k, inv_accept, &r->inv);
        if (!r->hl->dead) host_hands(m, r, k);
    }
    m->step++;
    /* Players waiting for their terrain are told too, so they see where
     * they are. */
    for (int id = 1; id < NET_MAX_PLAYERS; id++) {
        if (!m->r[id].active) continue;
        if (m->step % SNAP_EVERY == 0) host_snapshot(m, id);
        if (m->step % HEALTH_EVERY == 0) {
            uint8_t *buf = scratch(m, HEALTH_NET_MAX + 1);
            buf[0] = H_HEALTH;
            size_t len = health_encode(m->r[id].hl, buf + 1, HEALTH_NET_MAX);
            if (len) net_send(m->n, id, NET_CH_STATE, buf, len + 1);
        }
    }
}

static void host_flush(mp *m)
{
    /* Edits: the final block and meta at each changed cell. */
    for (int from = 0; from < m->edit_n;) {
        int count = m->edit_n - from > 1000 ? 1000 : m->edit_n - from;
        uint8_t *buf = scratch(m, 3 + (size_t)count * 11);
        wire_w w = wire_writer(buf, 3 + (size_t)count * 11);
        wire_u8(&w, H_EDITS);
        wire_u16(&w, (uint16_t)count);
        for (int i = from; i < from + count; i++) {
            ipos e = m->edits[i];
            wire_i32(&w, e.x);
            wire_u8(&w, (uint8_t)e.y);
            wire_i32(&w, e.z);
            wire_u8(&w, world_get(m->ctx.w, e.x, e.y, e.z));
            wire_u8(&w, world_get_meta(m->ctx.w, e.x, e.y, e.z));
        }
        net_broadcast(m->n, NET_CH_GAME, buf, w.len);
        from += count;
    }
    if (m->edit_n) memset(m->edit_set, 0, sizeof m->edit_set);
    m->edit_n = 0;

    for (int id = 1; id < NET_MAX_PLAYERS; id++) {
        remote *r = &m->r[id];
        if (!r->active) continue;
        r->col_tokens = fmin(COLUMN_BURST, r->col_tokens + (m->now - r->col_t) * COLUMN_RATE);
        r->col_t = m->now;
        for (int k = 0; k < COLUMNS_PER_FRAME && r->col_n && r->col_tokens >= 1.0; k++) {
            r->col_tokens -= 1.0;
            int cx = r->col[r->col_h][0], cz = r->col[r->col_h][1];
            r->col_h = (r->col_h + 1) % COLUMN_QUEUE;
            r->col_n--;
            uint8_t *buf = scratch(m, SAVE_MAX_FILE + 9);
            wire_w w = wire_writer(buf, 9);
            wire_u8(&w, H_COLUMN);
            wire_i32(&w, cx);
            wire_i32(&w, cz);
            size_t len = world_column_encode(m->ctx.w, cx, cz, buf + 9, SAVE_MAX_FILE);
            if (len) net_send(m->n, id, NET_CH_GAME, buf, len + 9);
        }
        /* Sent when it changed, and after every operation even if it did
         * not: a client that predicted a change waits for this to undo it. */
        uint8_t buf[5 + INV_ENCODED_SIZE] = {H_INV};
        if (inv_encode(&r->inv, buf + 5, INV_ENCODED_SIZE) &&
            (!r->inv_sent_valid || r->ops_told != r->ops_done || memcmp(buf + 5, r->inv_sent, INV_ENCODED_SIZE) != 0)) {
            for (int b = 0; b < 4; b++) buf[1 + b] = (uint8_t)(r->ops_done >> (8 * b));
            if (net_send(m->n, id, NET_CH_GAME, buf, sizeof buf) == 0) {
                memcpy(r->inv_sent, buf + 5, INV_ENCODED_SIZE);
                r->inv_sent_valid = 1;
                r->ops_told = r->ops_done;
            }
        }
        if (r->msg[0]) {
            uint8_t say[2 + sizeof r->msg] = {0};
            wire_w w = wire_writer(say, sizeof say);
            wire_u8(&w, H_MSG);
            wire_str(&w, r->msg, sizeof r->msg - 1);
            net_send(m->n, id, NET_CH_GAME, say, w.len);
            r->msg[0] = '\0';
        }
    }
}

/* ---------------------------------------------------------------- client */

static void column_request(void *user, int cx, int cz)
{
    mp *m = user;
    uint8_t buf[9] = {0};
    wire_w w = wire_writer(buf, sizeof buf);
    wire_u8(&w, C_COLUMN);
    wire_i32(&w, cx);
    wire_i32(&w, cz);
    net_send(m->n, 0, NET_CH_GAME, buf, w.len);
}

static void send_inputs(mp *m)
{
    client *c = &m->c;
    uint8_t buf[6 + INPUT_REDUNDANCY * 24] = {0};
    wire_w w = wire_writer(buf, sizeof buf);
    int count = c->seq < INPUT_REDUNDANCY ? (int)c->seq : INPUT_REDUNDANCY;
    wire_u8(&w, C_INPUT);
    wire_u32(&w, c->seq);
    wire_u8(&w, (uint8_t)count);
    for (int i = 0; i < count; i++) put_intent(&w, &c->hist[(c->seq - (uint32_t)i) % HISTORY]);
    if (count) net_send(m->n, 0, NET_CH_STATE, buf, w.len);
}

static void predict(mp *m, const intent *in, const player_input *raw)
{
    player *pl = m->ctx.pl;
    set_flying(pl, (in->buttons & B_FLY) != 0);
    player_input lim = *raw;
    survival_limit_input(m->ctx.hl, pl, &lim);
    physics_move_player(m->ctx.ph, pl, &lim);
}

void mp_client_step(mp *m, const player_input *in, int attack)
{
    client *c = &m->c;
    if (m->host || !mp_ready(m)) return;
    const player *pl = m->ctx.pl;
    intent it = {0};
    it.forward = in->forward;
    it.right = in->right;
    it.buttons = (uint8_t)((in->jump ? B_JUMP : 0) | (in->descend ? B_DESCEND : 0) | (in->sprint ? B_SPRINT : 0) |
                           (in->sneak ? B_SNEAK : 0) | (attack ? B_ATTACK : 0) | (pl->flying ? B_FLY : 0));
    it.yaw = pl->yaw;
    it.pitch = pl->pitch;
    it.selected = (uint8_t)m->ctx.inv->selected;
    memcpy(it.clicks, c->clicks, CLICKS);
    c->seq++;
    c->hist[c->seq % HISTORY] = it;
    c->raw[c->seq % HISTORY] = *in;
    predict(m, &it, in);
    send_inputs(m);
}

void mp_client_click(mp *m, mp_click c)
{
    if ((int)c >= 0 && c < CLICKS) m->c.clicks[c]++;
}

void mp_client_op(mp *m, mp_op op)
{
    if (m->host) return;
    uint8_t buf[5] = {C_OP, op.kind, op.a, op.b, op.c};
    if (net_send(m->n, 0, NET_CH_GAME, buf, sizeof buf) == 0) m->c.ops_sent++;
}

int mp_client_crack(const mp *m, ipos *at, float *progress)
{
    if (m->host || !m->c.cracking) return 0;
    *at = m->c.crack;
    *progress = m->c.crack_p;
    return 1;
}

static void client_edits(mp *m, wire_r *rd)
{
    int count = wire_get_u16(rd);
    for (int i = 0; i < count && !rd->bad; i++) {
        int x = wire_get_i32(rd), y = wire_get_u8(rd), z = wire_get_i32(rd);
        uint8_t id = wire_get_u8(rd), meta = wire_get_u8(rd);
        if (rd->bad || id >= B_COUNT || meta > 7 || y >= WORLD_H || llabs((long long)x) >= WORLD_LIMIT ||
            llabs((long long)z) >= WORLD_LIMIT) {
            rd->bad = 1;
            return;
        }
        /* A column not loaded here arrives later with the edit in it. */
        if (m->c.attached) world_set(m->ctx.w, x, y, z, id, meta);
    }
}

static void client_snapshot(mp *m, wire_r *rd)
{
    client *c = &m->c;
    uint32_t applied = wire_get_u32(rd);
    double day = wire_get_f64(rd);
    size_t pn = wire_get_u8(rd);
    uint8_t pb[PLAYER_NET_BYTES] = {0};
    if (pn > sizeof pb) rd->bad = 1;
    wire_get_bytes(rd, pb, rd->bad ? 0 : pn);
    player auth = *m->ctx.pl;
    if (rd->bad || player_decode(&auth, pb, pn) != 0) return;
    int cracking = wire_get_u8(rd);
    ipos crack = {wire_get_i32(rd), wire_get_u8(rd), wire_get_i32(rd)};
    float crack_p = fminf(fmaxf(wire_get_f32(rd), 0.0f), 1.0f);

    struct {
        dvec3 pos;
        int id;
        float yaw, pitch;
        uint8_t flags;
    } o[NET_MAX_PLAYERS] = {0};
    int others = wire_get_u8(rd);
    if (others >= NET_MAX_PLAYERS) rd->bad = 1;
    for (int i = 0; i < others && !rd->bad; i++) {
        o[i].id = wire_get_u8(rd);
        o[i].pos.x = wire_get_f64(rd);
        o[i].pos.y = wire_get_f64(rd);
        o[i].pos.z = wire_get_f64(rd);
        o[i].yaw = wire_get_f32(rd);
        o[i].pitch = wire_get_f32(rd);
        o[i].flags = wire_get_u8(rd);
        if (o[i].id >= NET_MAX_PLAYERS || fabs(o[i].pos.x) >= WORLD_LIMIT || fabs(o[i].pos.z) >= WORLD_LIMIT ||
            fabs(o[i].pos.y) > 1e5)
            rd->bad = 1;
    }
    /* Falling blocks and items: read into scratch first. */
    int nb = wire_get_u8(rd);
    body bodies[VIEW_ENTS] = {0};
    if (nb > VIEW_ENTS) rd->bad = 1;
    for (int i = 0; i < nb && !rd->bad; i++) {
        body *b = &bodies[i];
        b->pos = dv3(wire_get_f64(rd), wire_get_f64(rd), wire_get_f64(rd));
        for (int k = 0; k < 4; k++) b->rot[k] = wire_get_f32(rd);
        b->block = wire_get_u8(rd);
        b->prev_pos = b->pos;
        if (b->block >= B_COUNT || fabs(b->pos.x) >= WORLD_LIMIT || fabs(b->pos.z) >= WORLD_LIMIT ||
            fabs(b->pos.y) > 1e5)
            rd->bad = 1;
    }
    int ni = wire_get_u8(rd);
    item_ent items[VIEW_ENTS] = {0};
    if (ni > VIEW_ENTS) rd->bad = 1;
    for (int i = 0; i < ni && !rd->bad; i++) {
        item_ent *it = &items[i];
        it->pos = dv3(wire_get_f64(rd), wire_get_f64(rd), wire_get_f64(rd));
        it->spin = wire_get_f32(rd);
        it->id = wire_get_u8(rd);
        it->count = wire_get_u8(rd);
        it->prev_pos = it->pos;
        if (!item_valid(it->id) || fabs(it->pos.x) >= WORLD_LIMIT || fabs(it->pos.z) >= WORLD_LIMIT ||
            fabs(it->pos.y) > 1e5)
            rd->bad = 1;
    }
    int nm = wire_get_u8(rd);
    mob near[MOB_MAX] = {0};
    if (nm > MOB_MAX) rd->bad = 1;
    for (int i = 0; i < nm && !rd->bad; i++) {
        mob *mb = &near[i];
        mb->body.pos = dv3(wire_get_f64(rd), wire_get_f64(rd), wire_get_f64(rd));
        mb->body.yaw = wire_get_f32(rd);
        mb->stride = wire_get_f32(rd);
        mb->kind = wire_get_u8(rd);
        mb->hurt_t = wire_get_u8(rd) ? 0.1f : 0.0f;
        mb->body.prev_pos = mb->body.pos;
        if (mb->kind >= MOB_KINDS || fabs(mb->body.pos.x) >= WORLD_LIMIT || fabs(mb->body.pos.z) >= WORLD_LIMIT ||
            fabs(mb->body.pos.y) > 1e5 || !(fabsf(mb->body.yaw) < 1e4f) || !(fabsf(mb->stride) < 1e7f))
            rd->bad = 1;
    }
    if (rd->bad || rd->pos != rd->len || day < 0.0 || day > 1.0) return;
    memcpy(c->mobs.m, near, sizeof near[0] * (size_t)nm);
    c->mobs.count = nm;

    if (c->attached) {
        thermo_set_day_time(m->ctx.th, day);
        physics *ph = m->ctx.ph;
        memcpy(ph->bodies, bodies, sizeof bodies[0] * (size_t)nb);
        ph->body_count = nb;
        memcpy(ph->items, items, sizeof items[0] * (size_t)ni);
        ph->item_count = ni;
    }
    c->cracking = cracking != 0;
    c->crack = crack;
    c->crack_p = crack_p;

    /* Correct the prediction: the host's state at the last input it
     * applied, then the inputs it has not seen yet again. The view stays
     * where this player looks now. */
    player *pl = m->ctx.pl;
    float yaw = pl->yaw, pitch = pl->pitch;
    *pl = auth;
    if (c->attached && c->have_snap && (int32_t)(c->seq - applied) > 0 && c->seq - applied < HISTORY)
        for (uint32_t s = applied + 1; (int32_t)(c->seq - s) >= 0; s++)
            predict(m, &c->hist[s % HISTORY], &c->raw[s % HISTORY]);
    pl->yaw = yaw;
    pl->pitch = pitch;
    c->have_snap = 1;

    int seen[NET_MAX_PLAYERS] = {0};
    for (int i = 0; i < others; i++) {
        ghost *g = &c->g[o[i].id];
        seen[o[i].id] = 1;
        if (!g->present) {
            g->pos[1] = o[i].pos;
            g->yaw[1] = o[i].yaw;
            g->pitch[1] = o[i].pitch;
            g->t[1] = m->now;
        }
        g->pos[0] = g->pos[1];
        g->yaw[0] = g->yaw[1];
        g->pitch[0] = g->pitch[1];
        g->t[0] = g->t[1];
        g->pos[1] = o[i].pos;
        g->yaw[1] = o[i].yaw;
        g->pitch[1] = o[i].pitch;
        g->t[1] = m->now;
        g->flags = o[i].flags;
        g->present = 1;
    }
    for (int i = 0; i < NET_MAX_PLAYERS; i++)
        if (!seen[i]) c->g[i].present = 0;
}

static void client_message(mp *m, const uint8_t *data, size_t len)
{
    client *c = &m->c;
    wire_r rd = wire_reader(data, len);
    int type = wire_get_u8(&rd);
    switch (type) {
    case H_WELCOME: {
        uint32_t seed = wire_get_u32(&rd);
        double day = wire_get_f64(&rd);
        if (rd.bad || rd.pos != len || day < 0.0 || day > 1.0) return;
        c->seed = seed;
        c->welcomed = 1;
        if (c->attached) thermo_set_day_time(m->ctx.th, day);
    } break;
    case H_COLUMN: {
        int cx = wire_get_i32(&rd), cz = wire_get_i32(&rd);
        if (!rd.bad && c->attached) world_column_arrived(m->ctx.w, cx, cz, data + rd.pos, len - rd.pos);
    } break;
    case H_EDITS: client_edits(m, &rd); break;
    case H_SNAP: client_snapshot(m, &rd); break;
    case H_HEALTH:
        health_decode(m->ctx.hl, data + 1, len - 1); /* a malformed one is dropped */
        break;
    case H_INV: {
        uint32_t done = wire_get_u32(&rd);
        /* While operations of ours are on their way, the local copy is
         * ahead of this one: keep it. The hand stays where it is. */
        if (rd.bad || done != c->ops_sent) return;
        int sel = m->ctx.inv->selected;
        if (inv_decode(m->ctx.inv, data + rd.pos, len - rd.pos) == 0) m->ctx.inv->selected = sel;
    } break;
    case H_MSG: {
        char msg[sizeof c->msg] = {0};
        wire_get_str(&rd, msg, sizeof msg);
        if (!rd.bad && rd.pos == len) memcpy(c->msg, msg, sizeof msg);
    } break;
    default: break;
    }
}

/* ---------------------------------------------------------------- both */

void mp_receive(mp *m, int from, int ch, const uint8_t *data, size_t len)
{
    if (len < 1) return;
    if (!m->host) {
        if (from == 0) client_message(m, data, len);
        return;
    }
    if (from <= 0 || from >= NET_MAX_PLAYERS || !m->r[from].active) return;
    remote *r = &m->r[from];
    wire_r rd = wire_reader(data, len);
    int type = wire_get_u8(&rd);
    if (type == C_INPUT && ch == NET_CH_STATE) host_input(r, &rd);
    else if (type == C_OP && ch == NET_CH_GAME) host_op(m, r, &rd);
    else if (type == C_COLUMN && ch == NET_CH_GAME) host_column(r, &rd);
}

/* "Bob joined [1a2b3c4d]", with the key's fingerprint to compare out of
 * band, and a warning when Bob's key is not the one seen before. */
static void announce(mp *m, int id)
{
    const char *name = net_player_name(m->n, id);
    const uint8_t *key = net_player_key(m->n, id);
    char hex[2 * NC_KEY_BYTES + 1] = {0}, line[NET_CHAT_MAX + 1] = {0};
    if (!key) return;
    nc_key_hex(key, hex);
    snprintf(line, sizeof line, "%s joined [%.8s]", name, hex);
    chat_add(m, "", line);
    if (!m->ctx.known_players) return;
    char who[NETKEYS_HOST_MAX + 1] = {0};
    snprintf(who, sizeof who, "player:%s", name);
    for (char *c = who; *c; c++)
        if (*c == ' ') *c = '_';
    uint8_t seen[NC_KEY_BYTES] = {0};
    int known = netkeys_known(m->ctx.known_players, who, seen);
    if (known == 1 && memcmp(seen, key, NC_KEY_BYTES) != 0) {
        snprintf(line, sizeof line, "WARNING: %s has a different key than before; chat with them may not be private",
                 name);
        chat_add(m, "", line);
        log_warn("mp: %s", line);
    } else if (known == 0) {
        netkeys_pin(m->ctx.known_players, who, key);
    }
}

void mp_poll(mp *m, double now)
{
    m->now = now;
    net_event e;
    while (net_poll(m->n, now, &e)) {
        char line[NET_CHAT_MAX + 1] = {0};
        switch (e.type) {
        case NET_EV_JOINED:
            snprintf(m->names[e.player], sizeof m->names[0], "%s", net_player_name(m->n, e.player));
            if (net_player_key(m->n, e.player)) memcpy(m->keys[e.player], net_player_key(m->n, e.player), NC_KEY_BYTES);
            if (m->host) host_join(m, e.player);
            if (e.player != net_me(m->n)) announce(m, e.player);
            break;
        case NET_EV_LEFT:
            snprintf(line, sizeof line, "%s left", m->names[e.player]);
            if (m->host) host_leave(m, e.player, m->keys[e.player]);
            else m->c.g[e.player].present = 0;
            chat_add(m, "", line);
            break;
        case NET_EV_MESSAGE: mp_receive(m, e.player, e.ch, e.data, e.len); break;
        case NET_EV_CHAT: chat_add(m, net_player_name(m->n, e.player), e.text); break;
        case NET_EV_ENDED:
            m->ended = 1;
            snprintf(m->why, sizeof m->why, "%s", e.text);
            chat_add(m, "", e.text);
            break;
        default: break;
        }
    }
}

void mp_flush(mp *m, double now)
{
    m->now = now;
    if (m->host) host_flush(m);
    else if (m->c.attached && now - m->c.retry_t >= COLUMN_RETRY) {
        m->c.retry_t = now;
        world_retry_requests(m->ctx.w);
    }
    net_flush(m->n);
}

/* The safety numbers with one player (by name) or with everyone, as
 * notices on this side only. */
static void show_safety(mp *m, const char *name)
{
    int shown = 0;
    for (int id = 0; id < NET_MAX_PLAYERS; id++) {
        const uint8_t *theirs = net_player_key(m->n, id);
        const char *who = net_player_name(m->n, id);
        if (id == net_me(m->n) || !theirs || (*name && strcmp(name, who) != 0)) continue;
        char num[NC_SAFETY_CHARS] = {0}, line[NET_CHAT_MAX + 1] = {0};
        nc_safety_number(net_my_key(m->n), theirs, num);
        snprintf(line, sizeof line, "safety number with %s: %s", who, num);
        chat_add(m, "", line);
        shown++;
    }
    if (!shown) chat_add(m, "", *name ? "no player by that name" : "nobody else is here");
}

void mp_say(mp *m, const char *text)
{
    if (!net_chat_valid(text, strlen(text))) return;
    if (text[0] == '/') {
        if (strncmp(text, "/safety", 7) == 0 && (text[7] == '\0' || text[7] == ' '))
            show_safety(m, text[7] ? text + 8 : "");
        else chat_add(m, "", "commands: /safety [name] (read the number to that player in person or by voice)");
        return;
    }
    net_chat(m->n, text);
    chat_add(m, net_player_name(m->n, net_me(m->n)), text);
}

int mp_chat(const mp *m, const mp_chat_line **lines)
{
    *lines = m->chat;
    return m->chat_n;
}

uint32_t mp_chat_count(const mp *m) { return m->chat_total; }

const char *mp_take_message(mp *m)
{
    if (m->host || !m->c.msg[0]) return NULL;
    memcpy(m->c.taken, m->c.msg, sizeof m->c.taken);
    m->c.msg[0] = '\0';
    return m->c.taken;
}

int mp_avatars(const mp *m, double now, double alpha, mp_avatar *out, int max)
{
    int n = 0;
    for (int id = 0; id < NET_MAX_PLAYERS && n < max; id++) {
        if (id == net_me(m->n)) continue;
        mp_avatar *a = &out[n];
        memset(a, 0, sizeof *a);
        if (m->host) {
            const remote *r = &m->r[id];
            if (!r->active) continue;
            const player *p = &r->pl;
            a->pos = lerp3(p->prev_pos, p->pos, alpha);
            a->yaw = p->yaw;
            a->pitch = p->pitch;
            a->flying = p->flying;
            a->sneaking = p->sneaking;
            a->dead = r->hl->dead;
        } else {
            const ghost *g = &m->c.g[id];
            if (!g->present) continue;
            double span = g->t[1] - g->t[0];
            double f = span > 1e-6 ? (now - INTERP_DELAY - g->t[0]) / span : 1.0;
            f = f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f;
            a->pos = lerp3(g->pos[0], g->pos[1], f);
            a->yaw = g->yaw[1];
            a->pitch = g->pitch[1];
            a->flying = (g->flags & F_FLYING) != 0;
            a->sneaking = (g->flags & F_SNEAKING) != 0;
            a->dead = (g->flags & F_DEAD) != 0;
        }
        a->id = id;
        a->name = net_player_name(m->n, id);
        n++;
    }
    return n;
}
