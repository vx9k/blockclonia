/* libFuzzer target for a world database from a shared folder: the input
 * becomes world.db, which is opened (header, schema and format checks),
 * then its seed, the columns around the origin and the player are read
 * and decoded as the game would. Build with -DMC_BUILD_FUZZ=ON (clang). */
#include "mem.h"
#include "save.h"
#include "save_db.h"
#include "survival.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static char dir[256], path[320];
    static column *c;
    if (!c) {
        const char *tmp = getenv("TMPDIR");
        snprintf(dir, sizeof dir, "%s/mc_fuzz_db_XXXXXX", tmp && tmp[0] && strlen(tmp) < 200 ? tmp : "/tmp");
        if (!mkdtemp(dir)) abort();
        snprintf(path, sizeof path, "%s/world.db", dir);
        c = column_alloc(0, 0);
    }
    static const char *const SIDE[] = {"", "-wal", "-journal", "-shm"};
    for (size_t i = 0; i < sizeof SIDE / sizeof SIDE[0]; i++) {
        char p[340];
        snprintf(p, sizeof p, "%s%s", path, SIDE[i]);
        unlink(p);
    }
    if (save_write_file(path, data, size) != 0) return 0;

    save_db *db = save_db_open(dir);
    if (!db) return 0;
    uint32_t seed;
    save_db_get_seed(db, &seed);
    for (int cz = -1; cz <= 1; cz++)
        for (int cx = -1; cx <= 1; cx++) {
            uint8_t *bytes = NULL;
            long n = save_db_get_column(db, cx, cz, &bytes);
            c->cx = cx;
            c->cz = cz;
            if (n >= 0) save_decode_column(c, bytes, (size_t)n);
            mem_free(bytes);
        }
    static uint8_t buf[PLAYER_FILE_SIZE + 1];
    long n = save_db_get_blob(db, "player", buf, sizeof buf);
    if (n >= 0) {
        player p;
        inventory inv;
        double day;
        memset(&p, 0, sizeof p);
        memset(&inv, 0, sizeof inv);
        survival_decode_player(&p, &inv, &day, buf, (size_t)n);
    }
    save_db_close(db);
    return 0;
}
