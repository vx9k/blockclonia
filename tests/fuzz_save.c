/* libFuzzer target for the save-file decoder, the only code that parses
 * data from disk. Build with -DMC_BUILD_FUZZ=ON (clang). */
#include "save.h"
#include "world.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static column *c;
    if (!c) c = column_alloc(0, 0);
    save_decode_column(c, data, size);
    return 0;
}
