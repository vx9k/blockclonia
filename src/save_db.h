/* A world on disk: one SQLite database, <world>/world.db, holding the seed,
 * the columns the player changed and the player. Rows carry the same
 * encoded payloads the one-file-per-column format used (save.c encodes and
 * decodes them); this module only stores and finds them.
 *
 * World folders get shared, so the file is untrusted: it is opened without
 * following symlinks, with SQLite's defensive mode, untrusted schema and no
 * extensions, and refused unless its schema is exactly the one written
 * here. Callers still decode every row through the checked decoders.
 *
 * One connection, used from the main thread only: worker threads get the
 * bytes of a column from the main thread, never the database. */
#ifndef MC_SAVE_DB_H
#define MC_SAVE_DB_H

#include "save.h"
#include "world.h"
#include <stddef.h>
#include <stdint.h>

typedef struct save_db save_db;

/* Opens dir/world.db, creating it if missing. NULL, with the reason
 * logged, if it is not a regular file, not a blockclonia world of this
 * format, damaged, or open in another game. A refused file is left as it
 * was. */
save_db *save_db_open(const char *dir);
/* The page cache each database opened afterwards gets, in KB (sys_plan);
 * SQLite's own default (2 MB) until set. */
void save_db_set_cache_kb(int kb);
void save_db_close(save_db *db);

/* A batch of saves in one transaction, so it costs one commit and lands
 * whole or not at all. Not nested. commit returns -1 if it failed, and
 * then nothing written since begin is kept; rollback drops it on purpose. */
int save_db_begin(save_db *db);
int save_db_commit(save_db *db);
void save_db_rollback(save_db *db);

/* get_seed: 0 and *seed set if the world has one, -1 if it has none (or
 * not a valid one), -2 if it could not be read (a database error). */
int save_db_get_seed(save_db *db, uint32_t *seed);
int save_db_set_seed(save_db *db, uint32_t seed);
/* Integer facts about the world ("generator", "created", "last_played"):
 * get returns 0 with *v set, -1 if missing or not an integer, -2 on a
 * database error. */
int save_db_get_meta(save_db *db, const char *key, int64_t *v);
int save_db_set_meta(save_db *db, const char *key, int64_t v);

/* What the world list shows, read without changing anything: the file is
 * opened read-only with the same checks as save_db_open, nothing is
 * created or imported, and a world another game holds is reported, not
 * waited for. */
enum {
    SAVE_PEEK_OK,     /* a world of this format; the fields are set */
    SAVE_PEEK_NONE,   /* no world here */
    SAVE_PEEK_LEGACY, /* files an older build wrote; imported when played */
    SAVE_PEEK_IN_USE, /* open in a game (this one included) */
    SAVE_PEEK_BAD     /* refused: not ours, damaged, a newer format or a symlink */
};
typedef struct {
    int status;
    char name[SAVE_NAME_MAX + 1]; /* "" when the world has none (older worlds) */
    uint32_t seed;
    int has_seed;
    int generator;           /* terrain generator version; 1 when not recorded */
    int64_t last_played;     /* Unix seconds; 0 unknown */
} save_peek;
void save_db_peek(const char *dir, save_peek *p);

/* A column's encoded bytes. Returns the length, with *out a mem_alloc'd
 * copy to mem_free; -1 if the column was never saved; -2 if its row is
 * unusable (not a blob, or too big); -3 if it could not be read (a
 * database error), so the row may well be there. */
long save_db_get_column(save_db *db, int cx, int cz, uint8_t **out);
int save_db_put_column(save_db *db, const column *c);

/* Named blobs ("player"): get returns the length, -1 if missing, not a
 * blob or larger than cap, or -2 if it could not be read (a database
 * error). */
long save_db_get_blob(save_db *db, const char *name, void *buf, size_t cap);
int save_db_put_blob(save_db *db, const char *name, const void *data, size_t len);

/* Imports a world folder in the file format older builds wrote
 * (level.dat, player.dat, c.X.Z.bin) into a database that has no seed yet,
 * in one transaction. The files are read with save_read_file's checks and
 * left in place. Returns the files imported, 0 if there was nothing to
 * import (no valid level.dat, or the database already has a world), -1 on
 * a database error (nothing imported), including one reading the seed. */
int save_db_import(save_db *db, const char *dir);

#endif
