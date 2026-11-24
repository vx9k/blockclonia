#include "save_batch.h"
#include "mem.h"

#include <string.h>

void save_batch_init(save_batch *b) { memset(b, 0, sizeof *b); }

void save_batch_free(save_batch *b)
{
    for (size_t i = 0; i < b->n; i++) mem_free(b->ops[i].data);
    mem_free(b->ops);
    memset(b, 0, sizeof *b);
}

static int same_row(const save_op *o, save_op_kind kind, int cx, int cz, const char *key)
{
    if (o->kind != kind) return 0;
    return kind == SAVE_OP_COLUMN ? o->cx == cx && o->cz == cz : key && strcmp(o->key, key) == 0;
}

/* The row's place in the batch, or b->n if it has none. */
static size_t index_of(const save_batch *b, save_op_kind kind, int cx, int cz, const char *key)
{
    size_t i = 0;
    while (i < b->n && !same_row(&b->ops[i], kind, cx, cz, key)) i++;
    return i;
}

const save_op *save_batch_find(const save_batch *b, save_op_kind kind, int cx, int cz, const char *key)
{
    size_t i = index_of(b, kind, cx, cz, key);
    return i < b->n ? &b->ops[i] : NULL;
}

/* Takes data (mem_alloc'd) as the row's bytes. A row already in the batch
 * keeps its place and gets the new bytes, so finding stays a single match. */
static void put(save_batch *b, save_op_kind kind, int cx, int cz, const char *key, uint8_t *data, size_t len)
{
    size_t i = index_of(b, kind, cx, cz, key);
    if (i == b->n) {
        if (b->n == b->cap) {
            b->cap = b->cap ? b->cap * 2 : 16;
            b->ops = mem_realloc(b->ops, mem_array_size(b->cap, sizeof *b->ops));
        }
        b->ops[b->n] = (save_op){.kind = (uint8_t)kind, .cx = cx, .cz = cz};
        if (key) memcpy(b->ops[b->n].key, key, strlen(key) + 1);
        b->n++;
    } else {
        b->bytes -= b->ops[i].len;
        mem_free(b->ops[i].data);
    }
    save_op *o = &b->ops[i];
    o->data = data;
    o->len = len;
    b->bytes += len;
}

static int add(save_batch *b, save_op_kind kind, int cx, int cz, const char *key, const void *data, size_t len)
{
    if (key && (!key[0] || strlen(key) >= SAVE_KEY_MAX)) return -1;
    uint8_t *copy = mem_alloc(len ? len : 1);
    if (len) memcpy(copy, data, len);
    put(b, kind, cx, cz, key, copy, len);
    return 0;
}

int save_batch_column(save_batch *b, int cx, int cz, const uint8_t *data, size_t len)
{
    return add(b, SAVE_OP_COLUMN, cx, cz, NULL, data, len);
}

int save_batch_player(save_batch *b, const char *id, const void *data, size_t len)
{
    return add(b, SAVE_OP_PLAYER, 0, 0, id, data, len);
}

int save_batch_blob(save_batch *b, const char *name, const void *data, size_t len)
{
    return add(b, SAVE_OP_BLOB, 0, 0, name, data, len);
}

void save_batch_drop(save_batch *b, save_op_kind kind, int cx, int cz, const char *key)
{
    size_t i = index_of(b, kind, cx, cz, key);
    if (i == b->n) return;
    b->bytes -= b->ops[i].len;
    mem_free(b->ops[i].data);
    b->ops[i] = b->ops[--b->n];
}

void save_batch_merge(save_batch *into, save_batch *from)
{
    for (size_t i = 0; i < from->n; i++) {
        save_op *o = &from->ops[i];
        put(into, (save_op_kind)o->kind, o->cx, o->cz, o->kind == SAVE_OP_COLUMN ? NULL : o->key, o->data, o->len);
        o->data = NULL; /* now into's */
    }
    into->durable |= from->durable;
    from->n = 0;
    save_batch_free(from);
}
