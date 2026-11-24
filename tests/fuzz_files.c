/* libFuzzer target for the decoders of saved data: the player record
 * (player, inventory and whole body), the world's state record, a bare
 * inventory, and the settings file. Whatever a decoder accepts must
 * encode and decode back to the same values. Build with -DMC_BUILD_FUZZ=ON
 * (clang). */
#include "inventory.h"
#include "settings.h"
#include "survival.h"
#include "world_state.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t out[PLAYER_RECORD_MAX];
    static health body, again_body;
    player p = {0}, again_p = {0};
    inventory inv = {0}, again_inv = {0};
    memset(&p, 0, sizeof p);
    memset(&again_p, 0, sizeof again_p);
    inv_init(&inv);
    inv_init(&again_inv);
    health_init(&body, 1);
    health_init(&again_body, 1);
    /* What was accepted writes and reads back to the same values: the
     * fields a record lacked kept valid defaults. */
    if (survival_decode_player(&p, &inv, &body, data, size) == 0) {
        size_t n = survival_encode_player(&p, &inv, &body, out, sizeof out);
        if (!n || survival_decode_player(&again_p, &again_inv, &again_body, out, n) != 0 ||
            memcmp(&p, &again_p, sizeof p) != 0 || memcmp(&inv, &again_inv, sizeof inv) != 0 ||
            memcmp(&body, &again_body, sizeof body) != 0)
            abort();
    }
    thermo *th = thermo_create(1), *th2 = thermo_create(1);
    if (world_state_decode(th, data, size) == 0) {
        size_t n = world_state_encode(th, out, sizeof out);
        if (!n || world_state_decode(th2, out, n) != 0 || thermo_day_time(th) != thermo_day_time(th2) ||
            thermo_days(th) != thermo_days(th2))
            abort();
    }
    thermo_destroy(th);
    thermo_destroy(th2);
    if (inv_decode(&inv, data, size) == 0) {
        size_t n = inv_encode(&inv, out, sizeof out);
        inventory again = {0};
        if (n != INV_ENCODED_SIZE || (size == n && memcmp(out, data, size) != 0) || inv_decode(&again, out, n) != 0)
            abort();
    }

    /* Settings: what was parsed formats and parses back to the same. */
    static char text[SETTINGS_MAX_FILE + 1];
    size_t n = size < SETTINGS_MAX_FILE ? size : SETTINGS_MAX_FILE;
    memcpy(text, data, n);
    text[n] = '\0';
    settings s = {0}, t = {0};
    settings_default(&s);
    settings_parse(&s, text);
    settings_format(&s, text, (int)sizeof text);
    settings_default(&t);
    settings_parse(&t, text);
    if (memcmp(&s, &t, sizeof s) != 0) abort();
    return 0;
}
