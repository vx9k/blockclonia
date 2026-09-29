#include "save_db.h"
#include "log.h"
#include "mem.h"
#include "save.h"

#include <sqlite3.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* SQLITE_OPEN_NOFOLLOW and trusted_schema, which opening shared files
 * safely depends on, arrived in 3.31. */
#if defined(SQLITE_VERSION_NUMBER) && SQLITE_VERSION_NUMBER < 3031000
#error "SQLite 3.31 or newer is required"
#endif

/* The database header's application_id ("MCWL") and user_version say what
 * the file is before any table is read. FORMAT changes when the schema or
 * a row's meaning does. */
#define APP_ID 0x4D43574C
#define FORMAT 1

/* The whole schema. A file is used only if it holds exactly these tables
 * with exactly this SQL: no triggers, views or indexes, which is what a
 * crafted file would run code or hide data through. WITHOUT ROWID keeps
 * the key in the table itself, so there are no automatic indexes either. */
enum { TBL_META, TBL_COLUMNS, TBL_BLOBS, TBL_COUNT };
static const char *const TABLE_NAMES[TBL_COUNT] = {"meta", "columns", "blobs"};
static const char *const TABLE_SQL[TBL_COUNT] = {
    "CREATE TABLE meta(key TEXT PRIMARY KEY NOT NULL, value INTEGER NOT NULL) WITHOUT ROWID",
    ("CREATE TABLE columns(cx INTEGER NOT NULL, cz INTEGER NOT NULL, data BLOB NOT NULL, "
     "PRIMARY KEY(cx, cz)) WITHOUT ROWID"),
    "CREATE TABLE blobs(name TEXT PRIMARY KEY NOT NULL, data BLOB NOT NULL) WITHOUT ROWID",
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
static int schema_is_ours(sqlite3 *h, int *empty)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(h, "SELECT type, name, tbl_name, sql FROM sqlite_master", -1, &s, NULL) != SQLITE_OK) {
        sqlite3_finalize(s);
        return -1;
    }
    unsigned seen = 0;
    int rows = 0, ours = 1, rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        rows++;
        int k = -1;
        for (int i = 0; i < TBL_COUNT; i++)
            if (text_is(s, 0, "table") && text_is(s, 1, TABLE_NAMES[i]) && text_is(s, 2, TABLE_NAMES[i]) &&
                text_is(s, 3, TABLE_SQL[i]))
                k = i;
        if (k < 0 || (seen & 1u << k)) ours = 0;
        else seen |= 1u << k;
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return -1;
    *empty = rows == 0;
    return ours && seen == (1u << TBL_COUNT) - 1;
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

/* Checks the file is a world of this format, or makes it one if it is
 * new. Logs and returns -1 otherwise. */
static int check_or_create(sqlite3 *h, const char *path)
{
    sqlite3_int64 app = 0, version = 0;
    int empty = 0, ours = -1;
    if (pragma_int(h, "PRAGMA application_id", &app) == 0 && pragma_int(h, "PRAGMA user_version", &version) == 0)
        ours = schema_is_ours(h, &empty);
    if (ours < 0) {
        int busy = sqlite3_errcode(h) == SQLITE_BUSY || sqlite3_errcode(h) == SQLITE_LOCKED;
        log_error("%s: %s", path, busy ? "the world is open in another game" : sqlite3_errmsg(h));
        return -1;
    }
    if (empty && app == 0 && version == 0) {
        char sql[512];
        int n = snprintf(sql, sizeof sql,
                         "BEGIN; %s; %s; %s; PRAGMA application_id = %d; PRAGMA user_version = %d; COMMIT",
                         TABLE_SQL[0], TABLE_SQL[1], TABLE_SQL[2], APP_ID, FORMAT);
        if (n > 0 && (size_t)n < sizeof sql && exec(h, sql) == 0) return 0;
        log_error("%s: cannot create the world database: %s", path, sqlite3_errmsg(h));
        exec(h, "ROLLBACK");
        return -1;
    }
    if (app != APP_ID) log_error("%s is not a blockclonia world", path);
    else if (version != FORMAT) log_error("%s has world format %lld; this build reads %d", path, version, FORMAT);
    else if (!ours) log_error("%s has tables this build did not write; refusing it", path);
    return app == APP_ID && version == FORMAT && ours ? 0 : -1;
}

/* The database and the files SQLite keeps beside it must be regular files
 * or absent: a symlink planted as world.db-journal would otherwise have
 * SQLite write through it (older SQLite releases do not refuse one). */
static int side_files_ok(const char *path)
{
    static const char *const SUFFIX[] = {"", "-journal", "-wal", "-shm"};
    for (size_t i = 0; i < sizeof SUFFIX / sizeof SUFFIX[0]; i++) {
        char p[PATH_MAX + 32];
        struct stat st;
        int n = snprintf(p, sizeof p, "%s%s", path, SUFFIX[i]);
        if (n < 0 || (size_t)n >= sizeof p) return 0;
        if (lstat(p, &st) == 0 ? !S_ISREG(st.st_mode) : errno != ENOENT) {
            log_error("%s is not a regular file; refusing the world", p);
            return 0;
        }
    }
    return 1;
}

save_db *save_db_open(const char *dir)
{
    if (sqlite3_libversion_number() < 3031000) {
        log_error("SQLite %s is too old to open worlds safely (need 3.31)", sqlite3_libversion());
        return NULL;
    }
    /* SQLITE_OPEN_NOFOLLOW refuses a symlink anywhere in the path, and
     * homes are often reached through one (/home -> /var/home). Resolving
     * the folder first leaves only world.db itself to be refused. */
    char real[PATH_MAX], path[PATH_MAX + 16];
    if (!realpath(dir, real)) {
        log_error("world folder %s: %s", dir, strerror(errno));
        return NULL;
    }
    int n = snprintf(path, sizeof path, "%s/world.db", real);
    if (n < 0 || (size_t)n >= sizeof path || !side_files_ok(path)) return NULL;

    sqlite3 *h = NULL;
    int rc = sqlite3_open_v2(path, &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW, NULL);
    if (rc != SQLITE_OK || harden(h) != 0 || check_or_create(h, path) != 0) {
        if (rc != SQLITE_OK) log_error("%s: %s", path, h ? sqlite3_errmsg(h) : sqlite3_errstr(rc));
        sqlite3_close(h);
        return NULL;
    }
    /* Write-ahead logging: a save appends to world.db-wal and syncs only at
     * checkpoints, which keeps autosaves cheap on an SD card; a crash still
     * leaves every committed save. With the exclusive lock the WAL needs
     * no shared-memory file. A filesystem without WAL keeps the default
     * rollback journal. The first write also takes the exclusive lock. */
    exec(h, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL");
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

/* A blob result that fits in cap bytes, or -1. */
static long read_blob(sqlite3_stmt *s, void *buf, size_t cap)
{
    long r = -1;
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) == SQLITE_BLOB) {
        const void *p = sqlite3_column_blob(s, 0);
        int n = sqlite3_column_bytes(s, 0);
        if (n >= 0 && (size_t)n <= cap) {
            if (n) memcpy(buf, p, (size_t)n);
            r = n;
        }
    }
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return r;
}

int save_db_get_seed(save_db *db, uint32_t *seed)
{
    sqlite3_stmt *s = db->st[S_GET_META];
    int r = -1;
    sqlite3_bind_text(s, 1, "seed", -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) == SQLITE_INTEGER) {
        sqlite3_int64 v = sqlite3_column_int64(s, 0);
        if (v >= 0 && v <= (sqlite3_int64)UINT32_MAX) {
            *seed = (uint32_t)v;
            r = 0;
        }
    }
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return r;
}

int save_db_set_seed(save_db *db, uint32_t seed)
{
    sqlite3_stmt *s = db->st[S_PUT_META];
    sqlite3_bind_text(s, 1, "seed", -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)seed);
    return step_write(db, s);
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
    } else if (rc == SQLITE_ROW && sqlite3_column_type(s, 0) == SQLITE_BLOB) {
        const void *p = sqlite3_column_blob(s, 0);
        int n = sqlite3_column_bytes(s, 0);
        if (n >= 0 && n <= SAVE_MAX_FILE) {
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
    char path[512];
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
    uint32_t seed, have;
    if (save_db_get_seed(db, &have) == 0 || save_read_seed(dir, &seed) != 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    if (save_db_begin(db) != 0) {
        closedir(d);
        return -1;
    }
    int files = 1, failed = 0; /* level.dat */
    const struct dirent *e;
    while (!failed && (e = readdir(d)) != NULL) files += import_file(db, dir, e->d_name, &failed);
    closedir(d);
    if (!failed && save_db_set_seed(db, seed) == 0 && save_db_commit(db) == 0) return files;
    exec(db->h, "ROLLBACK");
    return -1;
}
