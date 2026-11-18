#include "save_db.h"
#include "log.h"
#include "mem.h"
#include "os.h"
#include "save.h"

#include <sqlite3.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Longest world folder path, resolved; Linux's PATH_MAX, fixed here so
 * the buffers do not shrink where PATH_MAX is smaller. */
#define DB_PATH_MAX 4096

/* SQLITE_OPEN_NOFOLLOW and trusted_schema, which opening shared files
 * safely depends on, arrived in 3.31; STRICT tables in 3.37. */
#if defined(SQLITE_VERSION_NUMBER) && SQLITE_VERSION_NUMBER < 3037000
#error "SQLite 3.37 or newer is required"
#endif

/* The database header's application_id ("MCWL") and user_version say what
 * the file is before any table is read. The format number changes when the
 * schema or a row's meaning does, and an older world is upgraded to it. */
#define APP_ID 0x4D43574C
#define FORMAT SAVE_DB_FORMAT

/* The bounds the schema's CHECKs hold rows to, spelled out because SQL
 * cannot see the C constants: column keys inside +-WORLD_LIMIT, and no
 * blob bigger than an encoded column. */
#define COL_KEY_MIN "-125000000"
#define COL_KEY_MAX "124999999"
#define DATA_MAX "393280"
_Static_assert(WORLD_LIMIT / CHUNK_W == 125000000, "update COL_KEY_MIN and COL_KEY_MAX");
_Static_assert(SAVE_MAX_FILE == 393280, "update DATA_MAX");

/* Every format's whole schema. A file is used only if it holds exactly the
 * tables of its format with exactly this SQL: no triggers, views or
 * indexes, which is what a crafted file would run code or hide data
 * through. WITHOUT ROWID keeps the key in the table itself, so there are
 * no automatic indexes either. From format 2 the tables are STRICT and
 * CHECK their rows, so SQLite itself refuses a wrongly typed or oversized
 * row; the decoders in save.c still check every field. */
#define TBL_MAX 3
typedef struct {
    int count;
    const char *name[TBL_MAX];
    const char *sql[TBL_MAX];
} schema;
static const schema SCHEMA[FORMAT + 1] = {
    [1] = {3,
           {"meta", "columns", "blobs"},
           {"CREATE TABLE meta(key TEXT PRIMARY KEY NOT NULL, value INTEGER NOT NULL) WITHOUT ROWID",
            ("CREATE TABLE columns(cx INTEGER NOT NULL, cz INTEGER NOT NULL, data BLOB NOT NULL, "
             "PRIMARY KEY(cx, cz)) WITHOUT ROWID"),
            "CREATE TABLE blobs(name TEXT PRIMARY KEY NOT NULL, data BLOB NOT NULL) WITHOUT ROWID"}},
    [2] = {3,
           {"meta", "columns", "blobs"},
           {("CREATE TABLE meta(key TEXT PRIMARY KEY NOT NULL CHECK(length(key) BETWEEN 1 AND 64), "
             "value INTEGER NOT NULL) STRICT, WITHOUT ROWID"),
            ("CREATE TABLE columns(cx INTEGER NOT NULL CHECK(cx BETWEEN " COL_KEY_MIN " AND " COL_KEY_MAX "), "
             "cz INTEGER NOT NULL CHECK(cz BETWEEN " COL_KEY_MIN " AND " COL_KEY_MAX "), "
             "data BLOB NOT NULL CHECK(length(data) BETWEEN 1 AND " DATA_MAX "), "
             "PRIMARY KEY(cx, cz)) STRICT, WITHOUT ROWID"),
            ("CREATE TABLE blobs(name TEXT PRIMARY KEY NOT NULL CHECK(length(name) BETWEEN 1 AND 80), "
             "data BLOB NOT NULL CHECK(length(data) <= " DATA_MAX ")) STRICT, WITHOUT ROWID")}},
};

enum { S_GET_COLUMN, S_PUT_COLUMN, S_GET_BLOB, S_PUT_BLOB, S_GET_META, S_PUT_META, S_COUNT };
static const char *const STMT_SQL[S_COUNT] = {
    "SELECT data FROM columns WHERE cx = ?1 AND cz = ?2",
    "INSERT OR REPLACE INTO columns(cx, cz, data) VALUES (?1, ?2, ?3)",
    "SELECT data FROM blobs WHERE name = ?1",
    "INSERT OR REPLACE INTO blobs(name, data) VALUES (?1, ?2)",
    "SELECT value FROM meta WHERE key = ?1",
    "INSERT OR REPLACE INTO meta(key, value) VALUES (?1, ?2)",
};

struct save_db {
    sqlite3 *h;
    sqlite3_stmt *st[S_COUNT];
    uint8_t *buf; /* SAVE_MAX_FILE bytes for encoding and importing */
};

static int exec(sqlite3 *h, const char *sql) { return sqlite3_exec(h, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1; }

/* The one-row integer result of a PRAGMA we wrote. */
static int pragma_int(sqlite3 *h, const char *sql, sqlite3_int64 *out)
{
    sqlite3_stmt *s = NULL;
    int ok = sqlite3_prepare_v2(h, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW &&
             sqlite3_column_type(s, 0) == SQLITE_INTEGER;
    if (ok) *out = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return ok ? 0 : -1;
}

/* A text column equal to want, compared by length too so a NUL inside the
 * stored text cannot cut it short. */
static int text_is(sqlite3_stmt *s, int col, const char *want)
{
    const unsigned char *t = sqlite3_column_text(s, col);
    return t && (size_t)sqlite3_column_bytes(s, col) == strlen(want) && strcmp((const char *)t, want) == 0;
}

/* 1 if the schema is exactly ours, 0 if not; *empty says whether there is
 * none at all (a new file). -1 if it cannot be read. */
static int schema_is_ours(sqlite3 *h, const schema *sc, int *empty)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(h, "SELECT type, name, tbl_name, sql FROM sqlite_master", -1, &s, NULL) != SQLITE_OK) {
        sqlite3_finalize(s);
        return -1;
    }
    unsigned seen = 0;
    int rows = 0, ours = 1, rc = 0;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        rows++;
        int k = -1;
        for (int i = 0; i < sc->count; i++)
            if (text_is(s, 0, "table") && text_is(s, 1, sc->name[i]) && text_is(s, 2, sc->name[i]) &&
                text_is(s, 3, sc->sql[i]))
                k = i;
        if (k < 0 || (seen & 1u << k)) ours = 0;
        else seen |= 1u << k;
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return -1;
    *empty = rows == 0;
    return ours && seen == (1u << sc->count) - 1;
}

/* What the security notes on sqlite.org advise for a database file that
 * came from someone else, set before the file is read. Returns 0 only if
 * the settings that matter took effect. */
static int harden(sqlite3 *h)
{
    int ok = sqlite3_db_config(h, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL) == SQLITE_OK;
    ok &= sqlite3_db_config(h, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL) == SQLITE_OK;
    ok &= sqlite3_db_config(h, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, NULL) == SQLITE_OK;
    ok &= sqlite3_db_config(h, SQLITE_DBCONFIG_ENABLE_VIEW, 0, NULL) == SQLITE_OK;
    ok &= sqlite3_db_config(h, SQLITE_DBCONFIG_DQS_DDL, 0, NULL) == SQLITE_OK;
    ok &= sqlite3_db_config(h, SQLITE_DBCONFIG_DQS_DML, 0, NULL) == SQLITE_OK;
    /* Off by default; said again in case the library was built otherwise.
     * A fetched build leaves extension loading out altogether. */
    sqlite3_db_config(h, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL);
    /* No row the game writes is bigger than a column file, and no SQL
     * longer than the schema above. */
    sqlite3_limit(h, SQLITE_LIMIT_LENGTH, SAVE_MAX_FILE + 1024);
    sqlite3_limit(h, SQLITE_LIMIT_SQL_LENGTH, 1024);
    sqlite3_limit(h, SQLITE_LIMIT_COLUMN, 16);
    sqlite3_limit(h, SQLITE_LIMIT_ATTACHED, 0);
    sqlite3_busy_timeout(h, 0);
    /* No memory map (a truncated or changed file could fault the game),
     * page checks as pages are read, and an exclusive lock for as long as
     * the game runs: a second game on the same world is refused instead
     * of both writing it. */
    ok &= exec(h, "PRAGMA trusted_schema = OFF; PRAGMA mmap_size = 0; PRAGMA cell_size_check = ON; "
                  "PRAGMA locking_mode = EXCLUSIVE") == 0;
    return ok ? 0 : -1;
}

/* Runs statements one at a time: SQLITE_LIMIT_SQL_LENGTH is short, and
 * applies to everything left in a string handed to exec. */
static int exec_all(sqlite3 *h, const char *const *sql, int n)
{
    for (int i = 0; i < n; i++)
        if (exec(h, sql[i]) != 0) return -1;
    return 0;
}

/* Rebuilds every table of format `from` as the next format's table of the
 * same name, copying the rows. Renaming the old table first keeps the new
 * one's SQL exactly as written (a renamed table's SQL is rewritten). A row
 * the new table's CHECKs refuse fails the whole upgrade. */
static int rebuild_tables(sqlite3 *h, int from)
{
    const schema *a = &SCHEMA[from], *b = &SCHEMA[from + 1];
    for (int i = 0; i < b->count; i++) {
        char sql[4][160] = {{0}};
        snprintf(sql[0], sizeof sql[0], "ALTER TABLE %s RENAME TO old_%s", a->name[i], a->name[i]);
        snprintf(sql[2], sizeof sql[2], "INSERT INTO %s SELECT * FROM old_%s", b->name[i], a->name[i]);
        snprintf(sql[3], sizeof sql[3], "DROP TABLE old_%s", a->name[i]);
        const char *steps[4] = {sql[0], b->sql[i], sql[2], sql[3]};
        if (exec_all(h, steps, 4) != 0) return -1;
    }
    return 0;
}

/* UPGRADE[v] takes a world from format v to v + 1, inside the upgrade's
 * one transaction. Format 2 made the tables STRICT with CHECKs. */
typedef int (*upgrade_fn)(sqlite3 *h, int from);
static const upgrade_fn UPGRADE[FORMAT] = {[1] = rebuild_tables};

/* Copies the open database into a new file at path (one that is not there
 * yet: never over anything, and never through a symlink). */
static int backup_to(sqlite3 *h, const char *path)
{
    if (os_path_kind(path) != OS_MISSING) return -1;
    sqlite3 *d = NULL;
    int rc = sqlite3_open_v2(path, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW, NULL);
    sqlite3_backup *b = rc == SQLITE_OK ? sqlite3_backup_init(d, "main", h, "main") : NULL;
    if (b) {
        rc = sqlite3_backup_step(b, -1);
        sqlite3_backup_finish(b);
    }
    int ok = b && rc == SQLITE_DONE;
    sqlite3_close(d);
    if (!ok) remove(path);
    return ok ? 0 : -1;
}

/* Upgrades a world of an older format, first copying it to
 * world.db.format<N> beside it (kept if an earlier try already made it).
 * Every step runs in one transaction, so a refused row or a full disk
 * leaves the file as it was. */
static int upgrade(sqlite3 *h, const char *path, int version)
{
    char bak[DB_PATH_MAX + 32] = {0};
    int n = snprintf(bak, sizeof bak, "%s.format%d", path, version);
    int kind = n > 0 && (size_t)n < sizeof bak ? os_path_kind(bak) : -1;
    if (kind != OS_FILE && (kind != OS_MISSING || backup_to(h, bak) != 0)) {
        log_error("%s: cannot keep a copy of the format %d world before upgrading it; not upgrading", path, version);
        return -1;
    }
    int ok = exec(h, "BEGIN IMMEDIATE") == 0;
    for (int v = version; ok && v < FORMAT; v++) ok = UPGRADE[v](h, v) == 0;
    char sql[64] = {0};
    snprintf(sql, sizeof sql, "PRAGMA user_version = %d", FORMAT);
    ok = ok && exec(h, sql) == 0;
    int empty = 0;
    ok = ok && schema_is_ours(h, &SCHEMA[FORMAT], &empty) == 1;
    if (!ok || exec(h, "COMMIT") != 0) {
        log_error("%s: upgrading from world format %d failed (%s); the world is unchanged", path, version,
                  sqlite3_errmsg(h));
        exec(h, "ROLLBACK");
        return -1;
    }
    /* Incremental vacuum (see save_db_close) can only be switched on by
     * rewriting the file; a failure here costs only the reclaiming. */
    sqlite3_limit(h, SQLITE_LIMIT_ATTACHED, 1); /* VACUUM builds the copy in an attached database */
    if (exec(h, "PRAGMA auto_vacuum = INCREMENTAL") != 0 || exec(h, "VACUUM") != 0)
        log_warn("%s: could not switch on incremental vacuum: %s", path, sqlite3_errmsg(h));
    sqlite3_limit(h, SQLITE_LIMIT_ATTACHED, 0);
    log_info("%s: upgraded from world format %d to %d (the old file is %s)", path, version, FORMAT, bak);
    return 0;
}

/* Checks the file is a world of a format this build reads, upgrading an
 * older one, or makes it one if it is new. Logs and returns -1 otherwise. */
static int check_or_create(sqlite3 *h, const char *path)
{
    sqlite3_int64 app = 0, version = 0;
    int empty = 0, ours = -1;
    if (pragma_int(h, "PRAGMA application_id", &app) == 0 && pragma_int(h, "PRAGMA user_version", &version) == 0)
        ours = schema_is_ours(h, &SCHEMA[version >= 1 && version <= FORMAT ? version : FORMAT], &empty);
    if (ours < 0) {
        int busy = sqlite3_errcode(h) == SQLITE_BUSY || sqlite3_errcode(h) == SQLITE_LOCKED;
        log_error("%s: %s", path, busy ? "the world is open in another game" : sqlite3_errmsg(h));
        return -1;
    }
    if (empty && app == 0 && version == 0) {
        /* auto_vacuum only takes effect before the first table. */
        char head[96] = {0}, tail[96] = {0};
        snprintf(head, sizeof head, "PRAGMA application_id = %d", APP_ID);
        snprintf(tail, sizeof tail, "PRAGMA user_version = %d", FORMAT);
        const schema *sc = &SCHEMA[FORMAT];
        const char *steps[] = {
            "PRAGMA auto_vacuum = INCREMENTAL", "BEGIN", sc->sql[0], sc->sql[1], sc->sql[2], head, tail, "COMMIT"};
        _Static_assert(TBL_MAX == 3, "list every table above");
        if (exec_all(h, steps, (int)(sizeof steps / sizeof steps[0])) == 0) return 0;
        log_error("%s: cannot create the world database: %s", path, sqlite3_errmsg(h));
        exec(h, "ROLLBACK");
        return -1;
    }
    if (app != APP_ID) log_error("%s is not a blockclonia world", path);
    else if (version < 1 || version > FORMAT)
        log_error("%s has world format %lld; this build reads up to %d", path, version, FORMAT);
    else if (!ours) log_error("%s has tables this build did not write; refusing it", path);
    if (app != APP_ID || version < 1 || version > FORMAT || !ours) return -1;
    return version < FORMAT ? upgrade(h, path, (int)version) : 0;
}

/* A quick structural check of every page (no index cross-checks): a
 * damaged file is refused with a clear reason, not found out mid-game. */
static int intact(sqlite3 *h, const char *path)
{
    sqlite3_stmt *s = NULL;
    int ok = sqlite3_prepare_v2(h, "PRAGMA quick_check(1)", -1, &s, NULL) == SQLITE_OK &&
             sqlite3_step(s) == SQLITE_ROW && text_is(s, 0, "ok");
    int busy = sqlite3_errcode(h) == SQLITE_BUSY || sqlite3_errcode(h) == SQLITE_LOCKED;
    const char *why = s && sqlite3_column_type(s, 0) == SQLITE_TEXT ? (const char *)sqlite3_column_text(s, 0)
                                                                    : sqlite3_errmsg(h);
    if (!ok && busy) log_error("%s: the world is open in another game", path);
    else if (!ok) log_error("%s is damaged (%s); refusing it", path, why);
    sqlite3_finalize(s);
    sqlite3_db_release_memory(h); /* the whole file passed through the cache; play reads little of it */
    return ok ? 0 : -1;
}

/* The database and the files SQLite keeps beside it must be regular files
 * or absent: a symlink planted as world.db-journal would otherwise have
 * SQLite write through it (older SQLite releases do not refuse one). */
static int side_files_ok(const char *path)
{
    static const char *const SUFFIX[] = {"", "-journal", "-wal", "-shm"};
    for (size_t i = 0; i < sizeof SUFFIX / sizeof SUFFIX[0]; i++) {
        char p[DB_PATH_MAX + 32] = {0};
        int n = snprintf(p, sizeof p, "%s%s", path, SUFFIX[i]);
        if (n < 0 || (size_t)n >= sizeof p) return 0;
        int kind = os_path_kind(p);
        if (kind != OS_FILE && kind != OS_MISSING) {
            log_error("%s is not a regular file; refusing the world", p);
            return 0;
        }
    }
    return 1;
}

static int g_cache_kb;

void save_db_set_cache_kb(int kb) { g_cache_kb = kb > 0 ? kb : 0; }

save_db *save_db_open(const char *dir)
{
    if (sqlite3_libversion_number() < 3037000) {
        log_error("SQLite %s is too old to open worlds safely (need 3.37)", sqlite3_libversion());
        return NULL;
    }
    /* SQLITE_OPEN_NOFOLLOW refuses a symlink anywhere in the path, and
     * homes are often reached through one (/home -> /var/home). Resolving
     * the folder first leaves only world.db itself to be refused. */
    char real[DB_PATH_MAX] = {0}, path[DB_PATH_MAX + 16] = {0};
    errno = 0;
    if (os_full_path(dir, real, sizeof real) != 0) {
        log_error("world folder %s: %s", dir, errno ? strerror(errno) : "cannot resolve it");
        return NULL;
    }
    int n = snprintf(path, sizeof path, "%s/world.db", real);
    if (n < 0 || (size_t)n >= sizeof path || !side_files_ok(path)) return NULL;

    sqlite3 *h = NULL;
    int rc = sqlite3_open_v2(path, &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW, NULL);
    if (rc != SQLITE_OK || harden(h) != 0 || intact(h, path) != 0 || check_or_create(h, path) != 0) {
        if (rc != SQLITE_OK) log_error("%s: %s", path, h ? sqlite3_errmsg(h) : sqlite3_errstr(rc));
        sqlite3_close(h);
        return NULL;
    }
    /* Write-ahead logging: a save appends to world.db-wal and syncs only at
     * checkpoints, which keeps autosaves cheap on an SD card. A crash of
     * the game keeps every committed save, but a power cut or an OS crash
     * can lose those since the last checkpoint (the file is not damaged:
     * the world opens as of an earlier commit). With the exclusive lock
     * the WAL needs no shared-memory file. A filesystem without WAL keeps
     * the default rollback journal. The first write also takes the
     * exclusive lock.
     * Persistent WAL stays off even where a build turns it on by default
     * (the system library on macOS appears to): closing then checkpoints
     * and deletes world.db-wal, so world.db alone is the world when it is
     * copied or shared. */
    int persist_wal = 0;
    sqlite3_file_control(h, "main", SQLITE_FCNTL_PERSIST_WAL, &persist_wal);
    exec(h, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL");
    if (g_cache_kb) {
        char pragma[64] = {0};
        snprintf(pragma, sizeof pragma, "PRAGMA cache_size = -%d", g_cache_kb); /* negative: KiB */
        exec(h, pragma);
    }
    if (exec(h, "BEGIN IMMEDIATE; COMMIT") != 0) {
        log_error("%s: %s", path,
                  sqlite3_errcode(h) == SQLITE_BUSY ? "the world is open in another game" : sqlite3_errmsg(h));
        sqlite3_close(h);
        return NULL;
    }

    save_db *db = mem_calloc(1, sizeof *db);
    db->h = h;
    for (int i = 0; i < S_COUNT; i++)
        if (sqlite3_prepare_v3(h, STMT_SQL[i], -1, SQLITE_PREPARE_PERSISTENT, &db->st[i], NULL) != SQLITE_OK) {
            log_error("%s: %s", path, sqlite3_errmsg(h));
            save_db_close(db);
            return NULL;
        }
    db->buf = mem_alloc(SAVE_MAX_FILE);
    return db;
}

void save_db_close(save_db *db)
{
    if (!db) return;
    /* The pages freed by replaced columns are handed back once they are a
     * quarter of the file. (No PRAGMA optimize: every lookup is by primary
     * key, and the sqlite_stat1 table ANALYZE would add is not part of the
     * schema a world may have.) */
    sqlite3_int64 free_pages = 0, pages = 0;
    if (pragma_int(db->h, "PRAGMA freelist_count", &free_pages) == 0 &&
        pragma_int(db->h, "PRAGMA page_count", &pages) == 0 && free_pages * 4 > pages)
        exec(db->h, "PRAGMA incremental_vacuum");
    for (int i = 0; i < S_COUNT; i++) sqlite3_finalize(db->st[i]);
    if (sqlite3_close(db->h) != SQLITE_OK) log_warn("world database did not close cleanly");
    mem_free(db->buf);
    mem_free(db);
}

int save_db_begin(save_db *db) { return exec(db->h, "BEGIN"); }

int save_db_commit(save_db *db)
{
    if (exec(db->h, "COMMIT") == 0) return 0;
    log_error("world save failed: %s", sqlite3_errmsg(db->h));
    exec(db->h, "ROLLBACK");
    return -1;
}

/* Also fine when SQLite rolled back on its own after a failed write. */
void save_db_rollback(save_db *db) { exec(db->h, "ROLLBACK"); }

/* Runs a prepared write to completion and resets it. */
static int step_write(save_db *db, sqlite3_stmt *s)
{
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    if (rc == SQLITE_DONE) return 0;
    log_error("world save failed: %s", sqlite3_errmsg(db->h));
    return -1;
}

/* A blob result that fits in cap bytes, -1 if there is none, or -2 if it
 * could not be read. */
static long read_blob(sqlite3_stmt *s, void *buf, size_t cap)
{
    long r = -1;
    int rc = sqlite3_step(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        r = -2;
    } else if (rc == SQLITE_ROW && sqlite3_column_type(s, 0) == SQLITE_BLOB) {
        const void *p = sqlite3_column_blob(s, 0);
        int n = sqlite3_column_bytes(s, 0);
        if (n > 0 && !p) r = -2; /* out of memory */
        else if (n >= 0 && (size_t)n <= cap) {
            if (n) memcpy(buf, p, (size_t)n);
            r = n;
        }
    }
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return r;
}

int save_db_get_meta(save_db *db, const char *key, int64_t *v)
{
    sqlite3_stmt *s = db->st[S_GET_META];
    int r = -1;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        log_error("cannot read the world's %s: %s", key, sqlite3_errmsg(db->h));
        r = -2;
    } else if (rc == SQLITE_ROW && sqlite3_column_type(s, 0) == SQLITE_INTEGER) {
        *v = (int64_t)sqlite3_column_int64(s, 0);
        r = 0;
    }
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return r;
}

int save_db_set_meta(save_db *db, const char *key, int64_t v)
{
    sqlite3_stmt *s = db->st[S_PUT_META];
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)v);
    return step_write(db, s);
}

int save_db_get_seed(save_db *db, uint32_t *seed)
{
    int64_t v = 0;
    int r = save_db_get_meta(db, "seed", &v);
    if (r != 0) return r;
    if (v < 0 || v > (int64_t)UINT32_MAX) return -1;
    *seed = (uint32_t)v;
    return 0;
}

int save_db_set_seed(save_db *db, uint32_t seed) { return save_db_set_meta(db, "seed", (int64_t)seed); }

/* One integer from meta on a bare connection (the peek has no prepared
 * statements); -1 missing or not an integer, -2 on an error. */
static int peek_meta(sqlite3 *h, const char *key, int64_t *v)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(h, STMT_SQL[S_GET_META], -1, &s, NULL) != SQLITE_OK) return -2;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s), r = -1;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) r = -2;
    else if (rc == SQLITE_ROW && sqlite3_column_type(s, 0) == SQLITE_INTEGER) {
        *v = (int64_t)sqlite3_column_int64(s, 0);
        r = 0;
    }
    sqlite3_finalize(s);
    return r;
}

void save_db_peek(const char *dir, save_peek *p)
{
    memset(p, 0, sizeof *p);
    p->generator = 1;
    p->status = SAVE_PEEK_BAD;
    char real[DB_PATH_MAX] = {0}, path[DB_PATH_MAX + 16] = {0};
    if (os_full_path(dir, real, sizeof real) != 0) return;
    int n = snprintf(path, sizeof path, "%s/world.db", real);
    if (n < 0 || (size_t)n >= sizeof path) return;
    int kind = os_path_kind(path);
    if (kind == OS_MISSING) {
        char old[DB_PATH_MAX + 16] = {0};
        n = snprintf(old, sizeof old, "%s/level.dat", real);
        p->status = n > 0 && (size_t)n < sizeof old && os_path_kind(old) == OS_FILE ? SAVE_PEEK_LEGACY : SAVE_PEEK_NONE;
        return;
    }
    if (kind != OS_FILE || !side_files_ok(path)) return;

    /* Read-only: the list never writes, creates or recovers anything. The
     * lock goes back to normal after harden: a read-only connection in
     * exclusive mode cannot read a WAL database (SQLite answers with an
     * I/O error). A world a game holds still answers busy, from that
     * game's own exclusive lock, instead of being read mid-write. */
    sqlite3 *h = NULL;
    int rc = sqlite3_open_v2(path, &h, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW, NULL);
    sqlite3_int64 app = 0, version = 0;
    int empty = 0, ours = -1;
    if (rc == SQLITE_OK && harden(h) == 0 && exec(h, "PRAGMA locking_mode = NORMAL") == 0 &&
        pragma_int(h, "PRAGMA application_id", &app) == 0 && pragma_int(h, "PRAGMA user_version", &version) == 0)
        ours = schema_is_ours(h, &SCHEMA[version >= 1 && version <= FORMAT ? version : FORMAT], &empty);
    if (ours < 0) {
        int busy = h && (sqlite3_errcode(h) == SQLITE_BUSY || sqlite3_errcode(h) == SQLITE_LOCKED);
        p->status = busy ? SAVE_PEEK_IN_USE : SAVE_PEEK_BAD;
        sqlite3_close(h);
        return;
    }
    if (app != APP_ID || version < 1 || version > FORMAT || !ours) {
        sqlite3_close(h);
        return;
    }
    int64_t v = 0;
    if (peek_meta(h, "seed", &v) == 0 && v >= 0 && v <= (int64_t)UINT32_MAX) {
        p->seed = (uint32_t)v;
        p->has_seed = 1;
    }
    if (peek_meta(h, "generator", &v) == 0 && v >= 1 && v <= 1000000) p->generator = (int)v;
    if (peek_meta(h, "last_played", &v) == 0 && v > 0) p->last_played = v;
    if (peek_meta(h, "mode", &v) == 0 && v >= 0 && v <= 3) p->mode = (int)v;
    if (peek_meta(h, "hardcore", &v) == 0 && v == 1) p->hardcore = 1;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(h, STMT_SQL[S_GET_BLOB], -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, "name", -1, SQLITE_STATIC);
        char name[SAVE_NAME_MAX + 1] = {0};
        long len = read_blob(s, name, SAVE_NAME_MAX);
        if (len > 0 && save_name_valid(name, (size_t)len)) {
            memcpy(p->name, name, (size_t)len);
            p->name[len] = '\0';
        }
        sqlite3_finalize(s);
    }
    sqlite3_close(h);
    p->status = SAVE_PEEK_OK;
}

long save_db_get_column(save_db *db, int cx, int cz, uint8_t **out)
{
    sqlite3_stmt *s = db->st[S_GET_COLUMN];
    sqlite3_bind_int(s, 1, cx);
    sqlite3_bind_int(s, 2, cz);
    long r = -2;
    int rc = sqlite3_step(s);
    if (rc == SQLITE_DONE) {
        r = -1;
    } else if (rc != SQLITE_ROW) {
        r = -3;
    } else if (sqlite3_column_type(s, 0) == SQLITE_BLOB) {
        const void *p = sqlite3_column_blob(s, 0);
        int n = sqlite3_column_bytes(s, 0);
        if (n > 0 && !p) r = -3; /* out of memory */
        else if (n >= 0 && n <= SAVE_MAX_FILE) {
            *out = mem_alloc(n ? (size_t)n : 1);
            if (n) memcpy(*out, p, (size_t)n);
            r = n;
        }
    }
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return r;
}

static int put_column_bytes(save_db *db, int cx, int cz, const uint8_t *data, size_t len)
{
    sqlite3_stmt *s = db->st[S_PUT_COLUMN];
    sqlite3_bind_int(s, 1, cx);
    sqlite3_bind_int(s, 2, cz);
    sqlite3_bind_blob(s, 3, data, (int)len, SQLITE_STATIC);
    return step_write(db, s);
}

int save_db_put_column(save_db *db, const column *c)
{
    size_t len = save_encode_column(c, db->buf, SAVE_MAX_FILE);
    return len ? put_column_bytes(db, c->cx, c->cz, db->buf, len) : -1;
}

long save_db_get_blob(save_db *db, const char *name, void *buf, size_t cap)
{
    sqlite3_stmt *s = db->st[S_GET_BLOB];
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    return read_blob(s, buf, cap);
}

int save_db_put_blob(save_db *db, const char *name, const void *data, size_t len)
{
    if (len > SAVE_MAX_FILE) return -1;
    sqlite3_stmt *s = db->st[S_PUT_BLOB];
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, data, (int)len, SQLITE_STATIC);
    return step_write(db, s);
}

/* Imports one file of an old world folder; 1 if it was. The bytes go in
 * as they are: loading decodes them like any other row. */
static int import_file(save_db *db, const char *dir, const char *name, int *failed)
{
    int cx = 0, cz = 0, player = strcmp(name, "player.dat") == 0;
    if (!player && !save_column_name(name, &cx, &cz)) return 0;
    char path[512] = {0};
    int n = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof path) return 0;
    /* Symlinks, FIFOs and oversized files stay behind: loading them from
     * the folder would have failed as well. */
    long len = save_read_file(path, db->buf, SAVE_MAX_FILE);
    if (len < 0) return 0;
    int r = player ? save_db_put_blob(db, "player", db->buf, (size_t)len)
                   : put_column_bytes(db, cx, cz, db->buf, (size_t)len);
    if (r != 0) *failed = 1;
    return r == 0;
}

int save_db_import(save_db *db, const char *dir)
{
    uint32_t seed = 0, have = 0;
    /* A seed that is there but could not be read must not let the old
     * files, which stay in the folder, be imported over the newer world. */
    int got = save_db_get_seed(db, &have);
    if (got == -2) return -1;
    if (got == 0 || save_read_seed(dir, &seed) != 0) return 0;
    os_dir *d = os_dir_open(dir);
    if (!d) return 0;
    if (save_db_begin(db) != 0) {
        os_dir_close(d);
        return -1;
    }
    int files = 1, failed = 0; /* level.dat */
    const char *name = NULL;
    while (!failed && (name = os_dir_next(d)) != NULL) files += import_file(db, dir, name, &failed);
    os_dir_close(d);
    if (!failed && save_db_set_seed(db, seed) == 0 && save_db_commit(db) == 0) return files;
    exec(db->h, "ROLLBACK");
    return -1;
}
