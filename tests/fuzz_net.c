/* libFuzzer target for what multiplayer peers send: the body and the
 * movement snapshots (whatever decodes must encode back to the same
 * bytes), chat text, sealed packets, and every message a host can send a
 * client. Build with -DMC_BUILD_FUZZ=ON (clang). */
#include "health.h"
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
    return 0;
}
