/* World persistence. Only columns the player changed are written, one file
 * per column, RLE-compressed. Files are untrusted input: the decoder
 * validates every field and rejects anything malformed. */
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

/* 1 = loaded, 0 = no save file, -1 = corrupt file. */
int save_load_column(const char *dir, column *c);
int save_store_column(const char *dir, const column *c);

int save_ensure_dir(const char *dir);
/* Reads/writes the world seed in <dir>/level.dat. read returns 0 if found. */
int save_read_seed(const char *dir, uint32_t *seed);
int save_write_seed(const char *dir, uint32_t seed);

#endif
