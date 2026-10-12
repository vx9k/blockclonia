/* libFuzzer target for the small decoders of saved data: the player record
 * (with the inventory inside it), a bare inventory, and the settings file.
 * Whatever the binary decoders accept must encode back to the same bytes,
 * so no byte of a record goes unchecked. Build with -DMC_BUILD_FUZZ=ON
 * (clang). */
#include "inventory.h"
#include "settings.h"
#include "survival.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t out[PLAYER_FILE_SIZE];
    player p = {0};
    inventory inv = {0};
    double day = 0.0;
    memset(&p, 0, sizeof p);
    if (survival_decode_player(&p, &inv, &day, data, size) == 0 &&
        (survival_encode_player(&p, &inv, day, out, sizeof out) != size || memcmp(out, data, size) != 0))
        abort();
    if (inv_decode(&inv, data, size) == 0 &&
        (inv_encode(&inv, out, sizeof out) != size || memcmp(out, data, size) != 0))
        abort();

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
