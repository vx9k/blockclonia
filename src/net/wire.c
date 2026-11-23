#include "wire.h"

#include <math.h>
#include <string.h>

static uint8_t *room(wire_w *w, size_t n)
{
    if (w->bad || n > w->cap - w->len) {
        w->bad = 1;
        return NULL;
    }
    uint8_t *p = w->p + w->len;
    w->len += n;
    return p;
}

static void put_le(wire_w *w, uint64_t v, int n)
{
    uint8_t *p = room(w, (size_t)n);
    if (!p) return;
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

void wire_u8(wire_w *w, uint8_t v) { put_le(w, v, 1); }
void wire_u16(wire_w *w, uint16_t v) { put_le(w, v, 2); }
void wire_u32(wire_w *w, uint32_t v) { put_le(w, v, 4); }
void wire_u64(wire_w *w, uint64_t v) { put_le(w, v, 8); }
void wire_i32(wire_w *w, int32_t v) { put_le(w, (uint32_t)v, 4); }

void wire_f32(wire_w *w, float v)
{
    uint32_t u = 0;
    memcpy(&u, &v, sizeof u);
    put_le(w, u, 4);
}

void wire_f64(wire_w *w, double v)
{
    uint64_t u = 0;
    memcpy(&u, &v, sizeof u);
    put_le(w, u, 8);
}

void wire_bytes(wire_w *w, const void *p, size_t n)
{
    uint8_t *d = room(w, n);
    if (d && n) memcpy(d, p, n);
}

void wire_str(wire_w *w, const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n > max || n > 255) n = max < 255 ? max : 255;
    wire_u8(w, (uint8_t)n);
    wire_bytes(w, s, n);
}

static const uint8_t *take(wire_r *r, size_t n)
{
    if (r->bad || n > r->len - r->pos) {
        r->bad = 1;
        return NULL;
    }
    const uint8_t *p = r->p + r->pos;
    r->pos += n;
    return p;
}

static uint64_t get_le(wire_r *r, int n)
{
    const uint8_t *p = take(r, (size_t)n);
    uint64_t v = 0;
    if (p)
        for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

uint8_t wire_get_u8(wire_r *r) { return (uint8_t)get_le(r, 1); }
uint16_t wire_get_u16(wire_r *r) { return (uint16_t)get_le(r, 2); }
uint32_t wire_get_u32(wire_r *r) { return (uint32_t)get_le(r, 4); }
uint64_t wire_get_u64(wire_r *r) { return get_le(r, 8); }
int32_t wire_get_i32(wire_r *r) { return (int32_t)(uint32_t)get_le(r, 4); }

float wire_get_f32(wire_r *r)
{
    uint32_t u = (uint32_t)get_le(r, 4);
    float v = 0.0f;
    memcpy(&v, &u, sizeof v);
    if (!isfinite(v)) {
        r->bad = 1;
        return 0.0f;
    }
    return r->bad ? 0.0f : v;
}

double wire_get_f64(wire_r *r)
{
    uint64_t u = get_le(r, 8);
    double v = 0.0;
    memcpy(&v, &u, sizeof v);
    if (!isfinite(v)) {
        r->bad = 1;
        return 0.0;
    }
    return r->bad ? 0.0 : v;
}

void wire_get_bytes(wire_r *r, void *out, size_t n)
{
    const uint8_t *p = take(r, n);
    if (p && n) memcpy(out, p, n);
    else if (n) memset(out, 0, n);
}

void wire_get_str(wire_r *r, char *out, size_t cap)
{
    size_t n = wire_get_u8(r);
    out[0] = '\0';
    if (r->bad || n >= cap) {
        r->bad = 1;
        return;
    }
    const uint8_t *p = take(r, n);
    if (!p) return;
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] == 0x7f) {
            r->bad = 1;
            return;
        }
    memcpy(out, p, n);
    out[n] = '\0';
}

int32_t wire_get_range(wire_r *r, int32_t lo, int32_t hi)
{
    int32_t v = wire_get_i32(r);
    if (v < lo || v > hi) {
        r->bad = 1;
        return 0;
    }
    return r->bad ? 0 : v;
}

void wire_put_fields(wire_w *w, const void *s, const wire_field *f, size_t n)
{
    const uint8_t *b = (const uint8_t *)s;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = b + f[i].off;
        switch (f[i].type) {
        case WF_F32: {
            float v = 0.0f;
            memcpy(&v, p, sizeof v);
            wire_f32(w, v);
        } break;
        case WF_F64: {
            double v = 0.0;
            memcpy(&v, p, sizeof v);
            wire_f64(w, v);
        } break;
        case WF_I32: {
            int32_t v = 0;
            memcpy(&v, p, sizeof v);
            wire_i32(w, v);
        } break;
        case WF_U32: {
            uint32_t v = 0;
            memcpy(&v, p, sizeof v);
            wire_u32(w, v);
        } break;
        default: wire_u8(w, *p); break;
        }
    }
}

void wire_get_fields(wire_r *r, void *s, const wire_field *f, size_t n)
{
    uint8_t *b = (uint8_t *)s;
    for (size_t i = 0; i < n && !r->bad; i++) {
        uint8_t *p = b + f[i].off;
        switch (f[i].type) {
        case WF_F32: {
            float v = wire_get_f32(r);
            memcpy(p, &v, sizeof v);
        } break;
        case WF_F64: {
            double v = wire_get_f64(r);
            memcpy(p, &v, sizeof v);
        } break;
        case WF_I32: {
            int32_t v = wire_get_range(r, f[i].lo, f[i].hi);
            memcpy(p, &v, sizeof v);
        } break;
        case WF_U32: {
            uint32_t v = wire_get_u32(r);
            memcpy(p, &v, sizeof v);
        } break;
        default: {
            uint8_t v = wire_get_u8(r);
            if (v < f[i].lo || v > f[i].hi) r->bad = 1;
            *p = v;
        } break;
        }
    }
}
