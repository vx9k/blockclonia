/* libFuzzer target for what multiplayer peers send: the body and the
 * movement snapshots (whatever decodes must encode back to the same
 * bytes), chat text, sealed packets, every message a host can send a
 * client, and every message players can send a host (inputs, inventory
 * operations, treatments, column requests), stepped through the host's
 * simulation. Build with -DMC_BUILD_FUZZ=ON (clang). */
#include "fx.h"
#include "health.h"
#include "jobs.h"
#include "mem.h"
#include "mp.h"
#include "net.h"
#include "netcrypto.h"
#include "physics.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define FUZZ_PLAYERS 3

/* A host without a socket, with three players. The input is a run of
 * messages, each [sender and channel][length][bytes], and the host
 * steps its simulation after each one. The world carries over between
 * inputs (edits pile up), the session does not. */
static void fuzz_host(const uint8_t *data, size_t size)
{
    static jobs *js;
    static world w;
    static physics ph;
    static thermo *th;
    static fx_state fx;
    static mobs mb;
    static combat cb;
    static player pl;
    static health *hl;
    static inventory inv;
    static nc_keypair kh, kp[FUZZ_PLAYERS];
    if (!js) {
        js = jobs_create(0);
        world_init(&w, 1, 2, js, NULL);
        world_load_blocking(&w, 0.5, 0.5, 3);
        physics_init(&ph, &w);
        th = thermo_create(1);
        fx_init(&fx, 1);
        hl = mem_calloc(1, sizeof *hl);
        nc_keypair_new(&kh);
        for (int i = 0; i < FUZZ_PLAYERS; i++) nc_keypair_new(&kp[i]);
    }
    player_spawn(&pl, &w, 0.5, 0.5);
    health_init_varied(hl, 1);
    inv_init(&inv);
    mobs_init(&mb, 1);
    combat_init(&cb, 1);
    mp_world ctx = {&w, &ph, th, &fx, &pl, hl, &inv, 1, NULL, &mb, &cb, NULL};
    net *n = net_offline_host(&kh, "Host");
    const char *names[FUZZ_PLAYERS] = {"A", "B", "C"};
    for (int i = 0; i < FUZZ_PLAYERS; i++) net_test_add_player(n, i + 1, names[i], kp[i].pk);
    mp *m = mp_create(n, &ctx);
    mp_poll(m, 0.0);
    const player_input idle = {0};
    for (size_t at = 0; at + 2 <= size;) {
        int from = data[at] % FUZZ_PLAYERS + 1, ch = data[at] & 0x80 ? NET_CH_STATE : NET_CH_GAME;
        size_t len = data[at + 1];
        at += 2;
        if (len > size - at) len = size - at;
        mp_receive(m, from, ch, data + at, len);
        at += len;
        mp_host_step(m, &idle);
        mp_flush(m, (double)at / 64.0);
    }
    mp_destroy(m);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static health *h, *k;
    static uint8_t out[HEALTH_NET_MAX];
    if (!h) {
        h = mem_calloc(1, sizeof *h);
        k = mem_calloc(1, sizeof *k);
        nc_init();
    }
    memset(h, 0, sizeof *h);
    if (health_decode(h, data, size) == 0) {
        /* The traces carry over from the destination, so a second decode
         * into a fresh body must give the same bytes back. */
        memset(k, 0, sizeof *k);
        size_t n = health_encode(h, out, sizeof out);
        if (n != size || memcmp(out, data, size) != 0 || health_decode(k, out, n) != 0) abort();
    }
    player p = {0};
    if (player_decode(&p, data, size) == 0 && (player_encode(&p, out, sizeof out) != size || memcmp(out, data, size)))
        abort();
    net_chat_valid((const char *)data, size);

    /* A packet sealed to nobody in particular never opens. */
    static nc_session s;
    long got = nc_open(&s, data, size, out, sizeof out, NULL);
    if (got >= 0) abort();

    /* Every message a host can send, at a client that is not connected. */
    static inventory inv;
    static player pl;
    mp_world ctx = {.pl = &pl, .hl = h, .inv = &inv};
    mp *m = mp_create(NULL, &ctx);
    mp_receive(m, 0, size % 2 ? NET_CH_GAME : NET_CH_STATE, data, size);
    mp_destroy(m);

    fuzz_host(data, size);
    return 0;
}
