/* World persistence: the encoding of a column (RLE-compressed; only
 * columns the player changed are saved), checked file access, and the
 * one-file-per-column format older builds kept worlds in, still read to
 * import them (save_db.h stores worlds now). Saved data is untrusted
 * input: the decoder validates every field and rejects anything
 * malformed. */
#ifndef MC_SAVE_H
#define MC_SAVE_H

#include <stddef.h>
#include <stdint.h>
#include "world.h"

#define SAVE_MAX_FILE (4 * COL_VOL + 64)

/* Encodes into `out` (capacity SAVE_MAX_FILE). Returns bytes written. */
size_t save_encode_column(const column *c, uint8_t *out, size_t cap);
/* Decodes into c (blocks array must exist; meta is (re)allocated as
 * needed). Returns 0 on success, -1 if the data is malformed. */
int save_decode_column(column *c, const uint8_t *data, size_t len);

/* 1 = loaded, 0 = no save file, -1 = corrupt or not a regular file. */
int save_load_column(const char *dir, column *c);
int save_store_column(const char *dir, const column *c);

/* Creates dir if it does not exist; 0 on success. */
int save_ensure_dir(const char *dir);
/* Reads/writes the world seed in <dir>/level.dat. read returns 0 if found. */
int save_read_seed(const char *dir, uint32_t *seed);
int save_write_seed(const char *dir, uint32_t seed);

/* Whole small files (settings, the player): read_file returns the length,
 * or -1 if the file is missing, not a regular file or larger than cap.
 * write_file goes through "<path>.tmp" and a rename. */
long save_read_file(const char *path, void *buf, size_t cap);
int save_write_file(const char *path, const void *data, size_t len);

/* Moving files from where older builds kept them: both copy through the
 * checks above, never overwrite, and leave the source alone.
 * copy_file copies a file of at most cap bytes: 1 copied, 0 nothing to
 * copy (from missing, unusable or too big, or `to` exists), -1 write
 * failed. copy_world copies a world folder's level.dat, player.dat and
 * c.X.Z.bin files into the existing directory `to` and skips anything
 * else: returns the files copied, 0 if `from` has no valid level.dat or
 * `to` has a level.dat or world.db (save_db.h) already. */
int save_copy_file(const char *from, const char *to, size_t cap);
int save_copy_world(const char *from, const char *to);

/* 1 and the coordinates if name is a column file name exactly as this
 * module writes it ("c.-3.12.bin"), so no other spelling (c.01.0.bin,
 * c.+1.0.bin) can stand in for a column. */
int save_column_name(const char *name, int *cx, int *cz);

#endif
