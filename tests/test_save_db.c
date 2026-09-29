/* The world database (save_db.c): round trips, refusing files that are not
 * a world of this format or are reached through symlinks, rows of the
 * wrong type or size, importing the old one-file-per-column format, and a
 * world saving and loading through it with a worker thread. */
#include "jobs.h"
#include "mem.h"
#include "save.h"
#include "save_db.h"
#include "test_util.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[256];

/* A fresh directory under the test root. */
static void sub_dir(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", g_root, name);
    (void)mkdir(out, 0700);
}

static void path_in(char *out, size_t cap, const char *dir, const char *name)
{ snprintf(out, cap, "%s/%s", dir, name); }

static long file_bytes(const char *path, uint8_t *buf, size_t cap) { return save_read_file(path, buf, cap); }

/* Runs SQL on the world file behind the game's back, as a crafted file
 * would be made. */
static int raw_sql(const char *dir, const char *sql)
{
    char p[512];
    path_in(p, sizeof p, dir, "world.db");
    sqlite3 *h = NULL;
    int ok = sqlite3_open(p, &h) == SQLITE_OK && sqlite3_exec(h, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_close(h);
    return ok;
}

static column *edited_column(int cx, int cz)
{
    column *c = column_alloc(cx, cz);
    test_flat_gen(0, c);
    c->blocks[col_index(3, GROUND, 4)] = B_PLANKS;
    c->meta = mem_calloc(COL_VOL, 1);
    c->blocks[col_index(5, GROUND, 5)] = B_WATER;
    c->meta[col_index(5, GROUND, 5)] = 3;
    return c;
}

static void test_roundtrip(void)
{
    char dir[400], p[512];
    sub_dir(dir, sizeof dir, "rt");
    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (!db) return;
    uint32_t seed = 0;
    CHECK(save_db_get_seed(db, &seed) == -1);
    CHECK(save_db_set_seed(db, 4000000000u) == 0);
    column *c = edited_column(-2, 7);
    CHECK(save_db_begin(db) == 0);
    CHECK(save_db_put_column(db, c) == 0);
    CHECK(save_db_put_blob(db, "player", "pos", 3) == 0);
    CHECK(save_db_commit(db) == 0);

    /* A second game on the same world is turned away. */
    CHECK(save_db_open(dir) == NULL);
    save_db_close(db);
    path_in(p, sizeof p, dir, "world.db-wal");
    CHECK(access(p, F_OK) != 0); /* checkpointed on close: world.db alone is the world */

    db = save_db_open(dir);
    CHECK(db != NULL);
    if (!db) return;
    CHECK(save_db_get_seed(db, &seed) == 0 && seed == 4000000000u);
    uint8_t *bytes = NULL;
    long n = save_db_get_column(db, -2, 7, &bytes);
    column *d = column_alloc(-2, 7);
    CHECK(n > 0 && save_decode_column(d, bytes, (size_t)n) == 0);
    CHECK(memcmp(c->blocks, d->blocks, COL_VOL) == 0 && d->meta && memcmp(c->meta, d->meta, COL_VOL) == 0);
    mem_free(bytes);
    bytes = NULL;
    CHECK(save_db_get_column(db, 7, -2, &bytes) == -1 && bytes == NULL);
    char buf[8];
    CHECK(save_db_get_blob(db, "player", buf, sizeof buf) == 3 && memcmp(buf, "pos", 3) == 0);
    CHECK(save_db_get_blob(db, "player", buf, 2) == -1); /* larger than the caller takes */
    CHECK(save_db_get_blob(db, "other", buf, sizeof buf) == -1);
    save_db_close(db);
    column_free(c);
    column_free(d);
}

/* open refuses the file and leaves it byte for byte as it was. */
static void check_refused(const char *dir)
{
    char p[512];
    path_in(p, sizeof p, dir, "world.db");
    uint8_t *before = mem_alloc(1 << 16), *after = mem_alloc(1 << 16);
    long a = file_bytes(p, before, 1 << 16);
    save_db *db = save_db_open(dir);
    CHECK(db == NULL);
    save_db_close(db);
    long b = file_bytes(p, after, 1 << 16);
    CHECK(a == b && (a < 0 || memcmp(before, after, (size_t)a) == 0));
    mem_free(before);
    mem_free(after);
}

/* A world of this build, then changed by raw SQL. */
static void make_world(char *dir, size_t cap, const char *name, const char *sql)
{
    sub_dir(dir, cap, name);
    save_db *db = save_db_open(dir);
    CHECK(db && save_db_set_seed(db, 1) == 0);
    save_db_close(db);
    if (sql) CHECK(raw_sql(dir, sql));
}

static void test_refused(void)
{
    char dir[400], p[512];

    sub_dir(dir, sizeof dir, "garbage");
    path_in(p, sizeof p, dir, "world.db");
    uint8_t junk[4096];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 131u + 7u);
    CHECK(save_write_file(p, junk, sizeof junk) == 0);
    check_refused(dir);

    sub_dir(dir, sizeof dir, "foreign");
    CHECK(raw_sql(dir, "CREATE TABLE notes(t TEXT); INSERT INTO notes VALUES ('mine')"));
    check_refused(dir);

    /* Our tables plus anything else, or another format number. */
    make_world(dir, sizeof dir, "trigger", "CREATE TRIGGER t AFTER INSERT ON columns BEGIN DELETE FROM meta; END");
    check_refused(dir);
    make_world(dir, sizeof dir, "index", "CREATE INDEX i ON blobs(data)");
    check_refused(dir);
    make_world(dir, sizeof dir, "view", "CREATE VIEW v AS SELECT 1");
    check_refused(dir);
    make_world(dir, sizeof dir, "altered",
               "DROP TABLE blobs; CREATE TABLE blobs(name TEXT PRIMARY KEY NOT NULL, data BLOB) WITHOUT ROWID");
    check_refused(dir);
    make_world(dir, sizeof dir, "newer", "PRAGMA user_version = 2");
    check_refused(dir);
    make_world(dir, sizeof dir, "app", "PRAGMA application_id = 7");
    check_refused(dir);

    /* Symlinks as the database or a file SQLite keeps beside it. */
    char victim[400];
    path_in(victim, sizeof victim, g_root, "victim.txt");
    CHECK(save_write_file(victim, "precious", 8) == 0);
    const char *names[] = {"world.db", "world.db-journal", "world.db-wal"};
    for (size_t i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof name, "link%zu", i);
        if (i == 0) sub_dir(dir, sizeof dir, name);
        else make_world(dir, sizeof dir, name, NULL);
        path_in(p, sizeof p, dir, names[i]);
        CHECK(symlink(victim, p) == 0);
        CHECK(save_db_open(dir) == NULL);
        char buf[16];
        CHECK(save_read_file(victim, buf, sizeof buf) == 8 && memcmp(buf, "precious", 8) == 0);
    }
}

static void test_bad_rows(void)
{
    char dir[400];
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO columns VALUES (0, 0, 'text'), (1, 0, x'00'), (2, 0, zeroblob(%d));"
             "INSERT OR REPLACE INTO meta VALUES ('seed', -5);"
             "INSERT INTO blobs VALUES ('player', 'text')",
             SAVE_MAX_FILE + 1);
    make_world(dir, sizeof dir, "rows", sql);
    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (!db) return;
    uint8_t *b = NULL;
    uint32_t seed;
    CHECK(save_db_get_column(db, 0, 0, &b) == -2 && b == NULL); /* not a blob */
    CHECK(save_db_get_column(db, 2, 0, &b) == -2 && b == NULL); /* too big */
    long n = save_db_get_column(db, 1, 0, &b);                   /* a blob, but no column */
    column *c = column_alloc(1, 0);
    CHECK(n == 1 && save_decode_column(c, b, (size_t)n) == -1);
    mem_free(b);
    column_free(c);
    CHECK(save_db_get_seed(db, &seed) == -1);
    CHECK(save_db_set_seed(db, 5) == 0 && save_db_get_seed(db, &seed) == 0 && seed == 5);
    char buf[16];
    CHECK(save_db_get_blob(db, "player", buf, sizeof buf) == -1);
    save_db_close(db);

    CHECK(raw_sql(dir, "INSERT OR REPLACE INTO meta VALUES ('seed', 4294967296)"));
    db = save_db_open(dir);
    CHECK(db && save_db_get_seed(db, &seed) == -1);
    save_db_close(db);
}

static void test_import(void)
{
    char dir[400], p[512], victim[400];
    sub_dir(dir, sizeof dir, "old");
    CHECK(save_write_seed(dir, 99) == 0);
    column *c = edited_column(0, -1);
    CHECK(save_store_column(dir, c) == 0);
    path_in(p, sizeof p, dir, "player.dat");
    CHECK(save_write_file(p, "PLAYER", 6) == 0);
    path_in(p, sizeof p, dir, "c.01.0.bin");
    CHECK(save_write_file(p, "alias", 5) == 0);
    path_in(victim, sizeof victim, g_root, "victim.txt");
    CHECK(save_write_file(victim, "precious", 8) == 0);
    path_in(p, sizeof p, dir, "c.3.3.bin");
    CHECK(symlink(victim, p) == 0);

    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (!db) return;
    CHECK(save_db_import(db, dir) == 3); /* level.dat, player.dat, c.0.-1.bin */
    uint32_t seed = 0;
    CHECK(save_db_get_seed(db, &seed) == 0 && seed == 99);
    uint8_t *b = NULL;
    long n = save_db_get_column(db, 0, -1, &b);
    column *d = column_alloc(0, -1);
    CHECK(n > 0 && save_decode_column(d, b, (size_t)n) == 0 && memcmp(c->blocks, d->blocks, COL_VOL) == 0);
    mem_free(b);
    b = NULL;
    CHECK(save_db_get_column(db, 1, 0, &b) == -1 && save_db_get_column(db, 3, 3, &b) == -1);
    char buf[16];
    CHECK(save_db_get_blob(db, "player", buf, sizeof buf) == 6 && memcmp(buf, "PLAYER", 6) == 0);
    CHECK(save_db_import(db, dir) == 0); /* once only */
    save_db_close(db);
    path_in(p, sizeof p, dir, "player.dat");
    CHECK(access(p, F_OK) == 0); /* the old files stay */

    /* A folder without a valid level.dat has nothing to import. */
    sub_dir(dir, sizeof dir, "none");
    db = save_db_open(dir);
    CHECK(db && save_db_import(db, dir) == 0 && save_db_get_seed(db, &seed) == -1);
    save_db_close(db);
    column_free(c);
    column_free(d);
}

/* The world fetches rows on the main thread and decodes them in a job. */
static void test_world_db(void)
{
    char dir[400];
    sub_dir(dir, sizeof dir, "world");
    save_db *db = save_db_open(dir);
    CHECK(db != NULL);
    if (!db) return;
    jobs *js = jobs_create(1);
    world w;
    world_init(&w, 1, 2, js, db);
    w.generator = test_flat_gen;
    world_load_blocking(&w, 0.5, 0.5, 1);
    CHECK(world_set(&w, 17, GROUND, -3, B_GLASS, 0));
    world_save_all(&w);
    const column *c = world_column(&w, 1, -1);
    CHECK(c && c->unsaved == 0);
    world_destroy(&w);

    world_init(&w, 1, 2, js, db);
    w.generator = test_flat_gen;
    world_load_blocking(&w, 0.5, 0.5, 1);
    CHECK(world_get(&w, 17, GROUND, -3) == B_GLASS && world_get(&w, 18, GROUND, -3) == B_AIR);
    world_destroy(&w);
    jobs_destroy(js);
    save_db_close(db);
}

/* Deletes what the tests made (one level of subfolders). */
static void clean_up(void)
{
    const char *subs[] = {"rt",  "garbage", "foreign", "trigger", "index", "view", "altered", "newer",
                          "app", "link0",   "link1",   "link2",   "rows",  "old",  "none",    "world"};
    const char *files[] = {"world.db",   "world.db-wal", "world.db-shm", "world.db-journal", "level.dat",
                           "player.dat", "c.0.-1.bin",   "c.01.0.bin",   "c.3.3.bin"};
    char d[400], p[512];
    for (size_t i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        path_in(d, sizeof d, g_root, subs[i]);
        for (size_t k = 0; k < sizeof files / sizeof files[0]; k++) {
            path_in(p, sizeof p, d, files[k]);
            unlink(p);
        }
        rmdir(d);
    }
    path_in(p, sizeof p, g_root, "victim.txt");
    unlink(p);
    rmdir(g_root);
}

void test_save_db_all(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_root, sizeof g_root, "%s/mc_db_XXXXXX", tmp && tmp[0] && strlen(tmp) < 200 ? tmp : "/tmp");
    int made = mkdtemp(g_root) != NULL;
    CHECK(made);
    if (!made) return;
    test_roundtrip();
    test_refused();
    test_bad_rows();
    test_import();
    test_world_db();
    clean_up();
}
