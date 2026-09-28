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

#include "world.h"
#include <stddef.h>
#include <stdint.h>

typedef struct save_db save_db;

/* Opens dir/world.db, creating it if missing. NULL, with the reason
 * logged, if it is not a regular file, not a blockclonia world of this
 * format, damaged, or open in another game. A refused file is left as it
 * was. */
save_db *save_db_open(const char *dir);
void save_db_close(save_db *db);

/* A batch of saves in one transaction, so it costs one commit and lands
 * whole or not at all. Not nested. commit returns -1 if it failed, and
 * then nothing written since begin is kept. */
int save_db_begin(save_db *db);
int save_db_commit(save_db *db);

/* get_seed: 0 and *seed set if the world has one, else -1. */
int save_db_get_seed(save_db *db, uint32_t *seed);
int save_db_set_seed(save_db *db, uint32_t seed);

/* A column's encoded bytes. Returns the length, with *out a mem_alloc'd
 * copy to mem_free; -1 if the column was never saved; -2 if its row is
 * unusable (not a blob, too big, or a database error). */
long save_db_get_column(save_db *db, int cx, int cz, uint8_t **out);
int save_db_put_column(save_db *db, const column *c);

/* Named blobs ("player"): get returns the length, or -1 if missing, not a
 * blob or larger than cap. */
long save_db_get_blob(save_db *db, const char *name, void *buf, size_t cap);
int save_db_put_blob(save_db *db, const char *name, const void *data, size_t len);

/* Imports a world folder in the file format older builds wrote
 * (level.dat, player.dat, c.X.Z.bin) into a database that has no seed yet,
 * in one transaction. The files are read with save_read_file's checks and
 * left in place. Returns the files imported, 0 if there was nothing to
 * import (no valid level.dat, or the database already has a world), -1 on
 * a database error (nothing imported). */
int save_db_import(save_db *db, const char *dir);

#endif
