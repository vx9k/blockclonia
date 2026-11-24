/* A batch of rows to write to a world's database in one transaction:
 * encoded columns, player records and named blobs, each a copy of its
 * bytes. The main thread fills one and hands it to the database's writer
 * thread (save_db_submit), which commits it while the game goes on.
 *
 * Batches waiting for the writer merge: a row written again replaces the
 * older bytes, so a backlog holds at most one copy of each row however
 * often it was saved. It deliberately knows nothing of SQLite or of
 * threads; save_db.c does the writing and the locking. No window or GPU. */
#ifndef MC_SAVE_BATCH_H
#define MC_SAVE_BATCH_H

#include <stddef.h>
#include <stdint.h>

typedef enum { SAVE_OP_COLUMN, SAVE_OP_PLAYER, SAVE_OP_BLOB } save_op_kind;

#define SAVE_KEY_MAX 81 /* the longest player id or blob name the tables take, with the terminator */

typedef struct {
    uint8_t kind;           /* save_op_kind */
    int32_t cx, cz;         /* SAVE_OP_COLUMN */
    char key[SAVE_KEY_MAX]; /* SAVE_OP_PLAYER and SAVE_OP_BLOB */
    uint8_t *data;
    size_t len;
} save_op;

typedef struct save_batch {
    save_op *ops;
    size_t n, cap;
    size_t bytes; /* of every op's data */
    int durable;  /* synced to the disk before the commit returns */
} save_batch;

void save_batch_init(save_batch *b);
void save_batch_free(save_batch *b); /* and empties it, ready for reuse */

/* Add a copy of the row, replacing an earlier op on the same row. Return
 * 0, or -1 for a key that is empty or too long. */
int save_batch_column(save_batch *b, int cx, int cz, const uint8_t *data, size_t len);
int save_batch_player(save_batch *b, const char *id, const void *data, size_t len);
int save_batch_blob(save_batch *b, const char *name, const void *data, size_t len);

/* The op on that row (key unused for columns), or NULL. */
const save_op *save_batch_find(const save_batch *b, save_op_kind kind, int cx, int cz, const char *key);

/* Removes the op on that row, if there is one. */
void save_batch_drop(save_batch *b, save_op_kind kind, int cx, int cz, const char *key);

/* Moves every op of from into into, the newer replacing the older on the
 * same row, and leaves from empty. into stays durable if either was. */
void save_batch_merge(save_batch *into, save_batch *from);

#endif
