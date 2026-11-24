#include "tagged.h"

#include <string.h>

/* A record: u16 field count, then per field its u32 key, u8 type, u16
 * element count and u32 byte length, then the elements, little-endian. A
 * TAG_SUB element is a record of its own. */

_Static_assert(sizeof(float) == 4 && sizeof(double) == 8, "TAG_F32 and TAG_F64 take 4 and 8 bytes");

uint32_t tag_key(const char *name)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = (h ^ *p) * 16777619u;
    return h;
}

static size_t elem_size(int type)
{
    switch (type) {
    case TAG_F64:
    case TAG_I64: return 8;
    case TAG_U8: return 1;
    case TAG_SUB: return 0;
    default: return 4;
    }
}

static void put_elem(wire_w *w, const uint8_t *p, int type)
{
    switch (type) {
    case TAG_F32: {
        float v = 0.0f;
        memcpy(&v, p, sizeof v);
        wire_f32(w, v);
    } break;
    case TAG_F64: {
        double v = 0.0;
        memcpy(&v, p, sizeof v);
        wire_f64(w, v);
    } break;
    case TAG_I64: {
        uint64_t v = 0;
        memcpy(&v, p, sizeof v);
        wire_u64(w, v);
    } break;
    case TAG_U8: wire_u8(w, *p); break;
    default: {
        uint32_t v = 0;
        memcpy(&v, p, sizeof v);
        wire_u32(w, v);
    } break;
    }
}

/* A field's header with a placeholder length; returns where the length goes. */
static size_t put_head(wire_w *w, const char *name, int type, uint16_t count)
{
    wire_u32(w, tag_key(name));
    wire_u8(w, (uint8_t)type);
    wire_u16(w, count);
    size_t at = w->len;
    wire_u32(w, 0);
    return at;
}

static void put_len(wire_w *w, size_t at)
{
    if (w->bad) return;
    uint32_t len = (uint32_t)(w->len - at - 4);
    for (int k = 0; k < 4; k++) w->p[at + (size_t)k] = (uint8_t)(len >> (8 * k));
}

/* Recursion through TAG_SUB goes only as deep as the build's own tables
 * nest, never as deep as the bytes ask. */
// NOLINTNEXTLINE(misc-no-recursion)
void tag_put(wire_w *w, const void *s, const tag_table *t)
{
    const uint8_t *b = (const uint8_t *)s;
    wire_u16(w, (uint16_t)t->n);
    for (size_t i = 0; i < t->n; i++) {
        const tag_field *f = &t->f[i];
        size_t at = put_head(w, f->name, f->type, f->count);
        for (uint32_t k = 0; k < f->count; k++) {
            if (f->type == TAG_SUB) tag_put(w, b + f->off + (size_t)k * f->stride, f->sub);
            else put_elem(w, b + f->off + k * elem_size(f->type), f->type);
        }
        put_len(w, at);
    }
}

// NOLINTNEXTLINE(misc-no-recursion): as deep as the tables nest
int tag_keys_distinct(const tag_table *t)
{
    if (t->n > TAG_MAX_FIELDS) return 0;
    for (size_t i = 0; i < t->n; i++) {
        for (size_t k = i + 1; k < t->n; k++)
            if (tag_key(t->f[i].name) == tag_key(t->f[k].name)) return 0;
        if (t->f[i].type == TAG_SUB && !tag_keys_distinct(t->f[i].sub)) return 0;
    }
    return 1;
}

void tag_put_sections(wire_w *w, const tag_section *sec, size_t n)
{
    uint16_t used = 0;
    for (size_t i = 0; i < n; i++) used = (uint16_t)(used + (sec[i].src != NULL));
    wire_u16(w, used);
    for (size_t i = 0; i < n; i++) {
        if (!sec[i].src) continue;
        size_t at = put_head(w, sec[i].name, TAG_SUB, 1);
        tag_put(w, sec[i].src, sec[i].t);
        put_len(w, at);
    }
}

void tag_get_sections(wire_r *r, const tag_section *sec, size_t n)
{
    uint32_t fields = wire_get_u16(r);
    for (uint32_t i = 0; i < fields && !r->bad; i++) {
        uint32_t key = wire_get_u32(r);
        uint8_t type = wire_get_u8(r);
        uint32_t count = wire_get_u16(r);
        uint32_t len = wire_get_u32(r);
        if (r->bad || len > r->len - r->pos) {
            r->bad = 1;
            return;
        }
        wire_r one = wire_reader(r->p + r->pos, len);
        r->pos += len;
        for (size_t k = 0; k < n; k++)
            if (sec[k].dst && type == TAG_SUB && count >= 1 && tag_key(sec[k].name) == key) {
                tag_get(&one, sec[k].dst, sec[k].t);
                if (one.bad) r->bad = 1;
                break;
            }
    }
}

void tag_put_empty(wire_w *w) { wire_u16(w, 0); }

static int in_range(const tag_field *f, double v) { return f->lo == f->hi || (v >= f->lo && v <= f->hi); }

static void get_elem(wire_r *r, uint8_t *p, const tag_field *f)
{
    double v = 0.0;
    switch (f->type) {
    case TAG_F32: {
        float x = wire_get_f32(r);
        memcpy(p, &x, sizeof x);
        v = (double)x;
    } break;
    case TAG_F64: {
        double x = wire_get_f64(r);
        memcpy(p, &x, sizeof x);
        v = x;
    } break;
    case TAG_I32: {
        int32_t x = wire_get_i32(r);
        memcpy(p, &x, sizeof x);
        v = (double)x;
    } break;
    case TAG_U32: {
        uint32_t x = wire_get_u32(r);
        memcpy(p, &x, sizeof x);
        v = (double)x;
    } break;
    case TAG_I64: {
        uint64_t u = wire_get_u64(r);
        int64_t x = 0;
        memcpy(&x, &u, sizeof x);
        memcpy(p, &x, sizeof x);
        v = (double)x;
    } break;
    default: {
        uint8_t x = wire_get_u8(r);
        *p = x;
        v = (double)x;
    } break;
    }
    if (!in_range(f, v)) r->bad = 1;
}

/* One field's elements from its own reader, which holds exactly them. */
// NOLINTNEXTLINE(misc-no-recursion): as deep as the tables nest
static void get_field(wire_r *r, uint8_t *b, const tag_field *f, uint32_t count)
{
    uint32_t keep = count < f->count ? count : f->count;
    if (f->type == TAG_SUB) {
        /* Elements past the table's are left unread in r. */
        for (uint32_t k = 0; k < keep && !r->bad; k++) tag_get(r, b + f->off + (size_t)k * f->stride, f->sub);
        return;
    }
    size_t size = elem_size(f->type);
    if (r->len != (size_t)count * size) {
        r->bad = 1;
        return;
    }
    for (uint32_t k = 0; k < keep && !r->bad; k++) get_elem(r, b + f->off + k * size, f);
}

// NOLINTNEXTLINE(misc-no-recursion): as deep as the tables nest
void tag_get(wire_r *r, void *s, const tag_table *t)
{
    uint8_t *b = (uint8_t *)s;
    uint32_t keys[TAG_MAX_FIELDS] = {0};
    size_t n = t->n < TAG_MAX_FIELDS ? t->n : TAG_MAX_FIELDS;
    for (size_t i = 0; i < n; i++) keys[i] = tag_key(t->f[i].name);
    uint32_t fields = wire_get_u16(r);
    for (uint32_t i = 0; i < fields && !r->bad; i++) {
        uint32_t key = wire_get_u32(r);
        uint8_t type = wire_get_u8(r);
        uint32_t count = wire_get_u16(r);
        uint32_t len = wire_get_u32(r);
        if (r->bad || len > r->len - r->pos) {
            r->bad = 1;
            return;
        }
        wire_r one = wire_reader(r->p + r->pos, len);
        r->pos += len;
        const tag_field *f = NULL;
        for (size_t k = 0; k < n && !f; k++)
            if (keys[k] == key && t->f[k].type == type) f = &t->f[k];
        if (!f) continue; /* a field this build does not have, or whose type changed */
        get_field(&one, b, f, count);
        if (one.bad) r->bad = 1;
    }
}
