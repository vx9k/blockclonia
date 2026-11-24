/* Tagged records: a struct written as a list of named fields, for saves
 * that must outlive the code that wrote them and for snapshots sent
 * between builds. Each field goes out as its key (a hash of its name),
 * its type, its element count and its length in bytes, then the values.
 *
 * A reader takes the fields its table knows and skips the rest, so a
 * newer build's extra fields cost an older one nothing. A field the table
 * knows but the bytes lack keeps whatever the destination already holds,
 * so the caller fills it with defaults first: a save written before a
 * feature existed loads with that feature at its defaults. An array that
 * grew or shrank keeps the elements both sides have, and a field whose
 * type changed is treated as missing. A struct inside a struct is a field
 * of type TAG_SUB with a table of its own; a save's sections are such
 * fields of the top-level record.
 *
 * It deliberately has no notion of meaning: ranges are the only checks it
 * makes, and the caller validates whatever spans several fields. Renaming
 * a struct member changes its key, so the old data stops loading; keep the
 * old name with TAG_AS when renaming one.
 *
 * Reading is for untrusted bytes, through wire.h: a field that runs past
 * its record, a float that is not finite and a number outside its field's
 * range mark the reader bad, and the caller keeps nothing. Nothing here
 * allocates, and it only descends into tables the build has, so how deep
 * it goes is fixed by the code, not by the bytes. */
#ifndef MC_TAGGED_H
#define MC_TAGGED_H

#include "wire.h"

#include <stddef.h>
#include <stdint.h>

typedef enum { TAG_F32, TAG_F64, TAG_I32, TAG_U32, TAG_U8, TAG_I64, TAG_SUB, TAG_TYPES } tag_type;

struct tag_table;
typedef struct {
    const char *name;
    uint32_t off;    /* offsetof the field */
    uint8_t type;    /* tag_type */
    uint16_t count;  /* elements: 1 for a scalar */
    uint32_t stride; /* bytes per element of a TAG_SUB array */
    double lo, hi;   /* allowed values (inclusive); lo == hi means any value of the type */
    const struct tag_table *sub;
} tag_field;

typedef struct tag_table {
    const tag_field *f;
    size_t n;
} tag_table;

#define TAG_MAX_FIELDS 256 /* fields in one table */

/* Elements of member f of T, each size bytes (1 for a scalar). */
#define TAG_ELEMS(T, f, size) ((uint16_t)(sizeof(((T *)0)->f) / (size)))
/* A field under another name than its member's (a renamed member); size
 * is the bytes of one element of the type. */
#define TAG_AS(name, T, f, type, size, lo, hi) {name, (uint32_t)offsetof(T, f), type, TAG_ELEMS(T, f, size), 0, lo, hi, NULL}
/* A scalar or an array of one type; arrays take their length from the
 * member. The R forms limit the values. */
#define TAG_F32(T, f) TAG_AS(#f, T, f, TAG_F32, 4u, 0, 0)
#define TAG_F32R(T, f, lo, hi) TAG_AS(#f, T, f, TAG_F32, 4u, lo, hi)
#define TAG_F64(T, f) TAG_AS(#f, T, f, TAG_F64, 8u, 0, 0)
#define TAG_F64R(T, f, lo, hi) TAG_AS(#f, T, f, TAG_F64, 8u, lo, hi)
#define TAG_I32(T, f) TAG_AS(#f, T, f, TAG_I32, 4u, 0, 0)
#define TAG_I32R(T, f, lo, hi) TAG_AS(#f, T, f, TAG_I32, 4u, lo, hi)
#define TAG_U32(T, f) TAG_AS(#f, T, f, TAG_U32, 4u, 0, 0)
#define TAG_U8(T, f) TAG_AS(#f, T, f, TAG_U8, 1u, 0, 0)
#define TAG_U8R(T, f, lo, hi) TAG_AS(#f, T, f, TAG_U8, 1u, lo, hi)
#define TAG_I64(T, f) TAG_AS(#f, T, f, TAG_I64, 8u, 0, 0)
#define TAG_I64R(T, f, lo, hi) TAG_AS(#f, T, f, TAG_I64, 8u, lo, hi)
/* A struct, or an array of them, of element type E described by table. */
#define TAG_SUB(T, f, E, table)                                                                                        \
    {#f, (uint32_t)offsetof(T, f), TAG_SUB, TAG_ELEMS(T, f, (size_t)sizeof(E)), (uint32_t)sizeof(E), 0, 0, &(table)}
#define TAG_TABLE(fields) {(fields), sizeof(fields) / sizeof((fields)[0])}

/* The key a field's name is written under (32-bit FNV-1a). */
uint32_t tag_key(const char *name);

/* Writes s as a record of every field in t. */
void tag_put(wire_w *w, const void *s, const tag_table *t);
/* Reads one record into s by t (see above). Marks r bad on malformed
 * bytes, leaving s partly written: decode into a scratch copy. */
void tag_get(wire_r *r, void *s, const tag_table *t);

/* 1 if no two fields of t, or of any table under it, share a key. Tests
 * call it on every table the build ships. */
int tag_keys_distinct(const tag_table *t);

/* A record whose fields are whole structs at separate addresses: a
 * save's sections. Written the same way as a TAG_SUB field of one element,
 * so a section can later move inside a table, or a table's field out into
 * a section, without changing the bytes. A section whose src (writing) or
 * dst (reading) is NULL is left out or skipped. */
typedef struct {
    const char *name;
    const void *src;
    void *dst;
    const tag_table *t;
} tag_section;
void tag_put_sections(wire_w *w, const tag_section *sec, size_t n);
void tag_get_sections(wire_r *r, const tag_section *sec, size_t n);

/* An empty record: a section kept for a system not built yet. */
void tag_put_empty(wire_w *w);

#endif
