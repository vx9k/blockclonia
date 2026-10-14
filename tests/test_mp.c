/* Multiplayer game state (mp.c) over loopback: a host and a client, each
 * with its own flat world. Terrain streams from the host with its edits,
 * edits made later follow, the client's predicted movement agrees with
 * the host's, inventory operations are applied by the host and wrong
 * predictions undone, chat arrives, garbage is shrugged off, and a player
 * leaving takes their avatar away. */
#include "mem.h"
#include "mp.h"
#include "os.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    test_world t;
    thermo *th;
    fx_state *fx;
    player pl;
    health *hl;
    inventory inv;
    mp *m;
} host_side;

typedef struct {
    jobs *js;
    world w;
    physics ph;
    thermo *th;
    player pl;
    health *hl;
    inventory inv;
    mp *m;
} client_side;

static void host_edit(void *user, int x, int y, int z, uint8_t old_id, uint8_t new_id)
{
    host_side *h = user;
    physics_on_block_changed(&h->t.ph, x, y, z, old_id, new_id);
    mp_on_block_changed(h->m, x, y, z);
}

/* One frame on both sides: steps, network, the client's streaming. */
static void frame(host_side *h, client_side *c, const player_input *move)
{
    const player_input idle = {0};
    double now = os_now();
    mp_poll(h->m, now);
    mp_poll(c->m, now);
    mp_host_step(h->m, &idle);
    if (c->w.grid) {
        mp_client_step(c->m, move ? move : &idle, 0);
        world_update(&c->w, c->pl.pos.x, c->pl.pos.y, c->pl.pos.z);
        jobs_poll(c->js, 0);
    }
    mp_flush(h->m, now);
    mp_flush(c->m, now);
    os_sleep(0.001);
}

static const mp_chat_line *last_chat(const mp *m)
{
    static const mp_chat_line none = {{0}, {0}, 0.0};
    const mp_chat_line *l = NULL;
    int n = mp_chat(m, &l);
    return n ? &l[n - 1] : &none;
}

static int avatar(const mp *m, int id, mp_avatar *out)
{
    mp_avatar a[NET_MAX_PLAYERS];
    int n = mp_avatars(m, os_now() + 1.0, 1.0, a, NET_MAX_PLAYERS);
    for (int i = 0; i < n; i++)
        if (a[i].id == id) {
            *out = a[i];
            return 1;
        }
    return 0;
}

void test_mp_all(void)
{
    CHECK(nc_init() == 0);
    static host_side h;
    static client_side c;
    memset(&h, 0, sizeof h);
    memset(&c, 0, sizeof c);

    tw_init(&h.t);
    h.th = thermo_create(1);
    h.fx = mem_alloc(sizeof *h.fx);
    fx_init(h.fx, 1);
    player_spawn(&h.pl, &h.t.w, 0.5, 0.5);
    h.hl = new_body(1);
    inv_init(&h.inv);
    nc_keypair kh = {0}, kc = {0};
    nc_keypair_new(&kh);
    nc_keypair_new(&kc);
    net *hn = NULL;
    uint16_t port = 0;
    for (int tries = 0; tries < 20 && !hn; tries++) {
        port = (uint16_t)(30000 + (getpid() * 13 + tries * 197 + 7) % 30000);
        hn = net_host(&kh, "Host", port);
    }
    CHECK(hn != NULL);
    if (!hn) return;
    mp_world hw = {&h.t.w, &h.t.ph, h.th, h.fx, &h.pl, h.hl, &h.inv, 1};
    h.m = mp_create(hn, &hw);
    h.t.w.edit_user = &h;
    h.t.w.on_block_changed = host_edit;
    CHECK(mp_is_host(h.m) && mp_ready(h.m));

    /* A block placed before the client arrives comes with the terrain. */
    CHECK(world_set(&h.t.w, 3, GROUND, 3, B_PLANKS, 0));

    c.js = jobs_create(0);
    c.th = thermo_create(1);
    c.hl = new_body(2);
    inv_init(&c.inv);
    net *cn = net_join(&kc, "Client", "127.0.0.1", port, 0, NULL);
    CHECK(cn != NULL);
    mp_world cw = {&c.w, &c.ph, c.th, NULL, &c.pl, c.hl, &c.inv, 0};
    c.m = mp_create(cn, &cw);
    uint32_t seed = 0;
    for (int i = 0; i < 3000 && !mp_welcome(c.m, &seed); i++) frame(&h, &c, NULL);
    CHECK(mp_welcome(c.m, &seed) && seed == 1);
    world_init(&c.w, seed, 2, c.js, NULL);
    c.w.generator = test_flat_gen; /* only if a column from the host fails to decode */
    physics_init(&c.ph, &c.w);
    mp_attach(c.m, &cw);
    for (int i = 0; i < 5000 && !(mp_ready(c.m) && world_column(&c.w, 0, 0)); i++) frame(&h, &c, NULL);
    CHECK(mp_ready(c.m) && world_column(&c.w, 0, 0) != NULL);
    CHECK(world_get(&c.w, 3, GROUND, 3) == B_PLANKS);
    CHECK(strstr(last_chat(h.m)->text, "Client joined") != NULL);

    /* An edit made now follows. */
    CHECK(world_set(&h.t.w, 4, GROUND, 4, B_STONE, 0));
    for (int i = 0; i < 200 && world_get(&c.w, 4, GROUND, 4) != B_STONE; i++) frame(&h, &c, NULL);
    CHECK(world_get(&c.w, 4, GROUND, 4) == B_STONE);

    /* Walking: the client predicts, the host simulates, they agree. */
    for (int i = 0; i < 30; i++) frame(&h, &c, NULL); /* settle on the ground */
    dvec3 start = c.pl.pos;
    player_input fwd = {.forward = 1.0f};
    for (int i = 0; i < 60; i++) frame(&h, &c, &fwd);
    for (int i = 0; i < 60; i++) frame(&h, &c, NULL);
    mp_avatar me = {0}, host = {0};
    CHECK(avatar(h.m, 1, &me));
    double moved = hypot(c.pl.pos.x - start.x, c.pl.pos.z - start.z);
    double apart = hypot(c.pl.pos.x - me.pos.x, c.pl.pos.z - me.pos.z);
    CHECK(moved > 1.0 && apart < 0.05);
    CHECK(avatar(c.m, 0, &host) && hypot(host.pos.x - h.pl.pos.x, host.pos.z - h.pl.pos.z) < 0.05);
    CHECK(strcmp(host.name, "Host") == 0);
    CHECK(c.hl->t > 1.0); /* the body arrives from the host */

    /* An inventory click goes through the host; a click it refuses (a
     * slot that does not exist) undoes what the client guessed. */
    for (int i = 0; i < 30; i++) frame(&h, &c, NULL);
    int slot = 0;
    while (slot < INV_SLOTS - 1 && !c.inv.slot[slot].count) slot++;
    item_stack first = c.inv.slot[slot];
    CHECK(first.count > 0); /* the starting kit, from the host */
    inv_click(&c.inv, slot, 0, 0);
    mp_client_op(c.m, (mp_op){MP_OP_CLICK, (uint8_t)slot, 0, 0});
    for (int i = 0; i < 60; i++) frame(&h, &c, NULL);
    CHECK(c.inv.cursor.id == first.id && c.inv.cursor.count == first.count && c.inv.slot[slot].count == 0);
    int sticks = inv_count(&c.inv, I_STICK);
    inv_add(&c.inv, I_STICK, 5);
    mp_client_op(c.m, (mp_op){MP_OP_CLICK, 200, 0, 0});
    for (int i = 0; i < 60; i++) frame(&h, &c, NULL);
    CHECK(inv_count(&c.inv, I_STICK) == sticks);

    /* Chat, end-to-end. */
    mp_say(c.m, "hello host");
    for (int i = 0; i < 200 && strcmp(last_chat(h.m)->text, "hello host") != 0; i++) frame(&h, &c, NULL);
    CHECK(strcmp(last_chat(h.m)->text, "hello host") == 0 && strcmp(last_chat(h.m)->who, "Client") == 0);
    CHECK(strcmp(last_chat(c.m)->who, "Client") == 0); /* shown to the sender too */

    /* Garbage in any message, either way, is refused without harm. */
    uint32_t rng = 12345;
    uint8_t junk[96] = {0};
    static const uint8_t TYPES[] = {1, 2, 3, 16, 17, 18, 19, 20, 21, 22, 0, 99};
    for (int i = 0; i < 3000; i++) {
        size_t len = 1 + (size_t)(i % 95);
        for (size_t k = 0; k < len; k++) {
            rng = rng * 1664525u + 1013904223u;
            junk[k] = (uint8_t)(rng >> 24);
        }
        junk[0] = TYPES[i % (int)sizeof TYPES];
        mp_receive(h.m, 1, i % 2 ? NET_CH_GAME : NET_CH_STATE, junk, len);
        mp_receive(c.m, 0, i % 2 ? NET_CH_GAME : NET_CH_STATE, junk, len);
    }
    for (int i = 0; i < 60; i++) frame(&h, &c, NULL);
    CHECK(!mp_ended(c.m, NULL) && avatar(h.m, 1, &me));
    CHECK(fabs(c.pl.pos.x) < WORLD_LIMIT && fabs(c.pl.pos.z) < WORLD_LIMIT);

    /* The client leaves. */
    mp_destroy(c.m);
    physics_destroy(&c.ph);
    world_destroy(&c.w);
    jobs_destroy(c.js);
    for (int i = 0; i < 2000 && avatar(h.m, 1, &me); i++) {
        mp_poll(h.m, os_now());
        os_sleep(0.001);
    }
    CHECK(!avatar(h.m, 1, &me));
    CHECK(strstr(last_chat(h.m)->text, "Client left") != NULL);

    mp_destroy(h.m);
    thermo_destroy(c.th);
    thermo_destroy(h.th);
    mem_free(c.hl);
    mem_free(h.hl);
    mem_free(h.fx);
    tw_free(&h.t);
}
