/* Byte encoding for multiplayer messages: a bounded writer and reader for
 * little-endian integers, IEEE floats and short strings, and tables that
 * describe a struct field by field so it can be sent between machines
 * whose compilers pad it differently.
 *
 * Reading is for untrusted bytes. Running past the end, a float that is
 * not finite, an integer outside its field's range or a string with a
 * control character marks the reader bad, and every later read returns
 * 0, so a decoder checks `bad` once at the end. Decoders fill a scratch
 * copy and the caller keeps it only when nothing went bad. */
#ifndef MC_WIRE_H
#define MC_WIRE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *p;
    size_t len, cap;
    int bad; /* ran out of room */
} wire_w;

typedef struct {
    const uint8_t *p;
    size_t len, pos;
    int bad;
} wire_r;

static inline wire_w wire_writer(uint8_t *p, size_t cap) { return (wire_w){p, 0, cap, 0}; }
static inline wire_r wire_reader(const uint8_t *p, size_t len) { return (wire_r){p, len, 0, 0}; }

void wire_u8(wire_w *w, uint8_t v);
void wire_u16(wire_w *w, uint16_t v);
void wire_u32(wire_w *w, uint32_t v);
void wire_u64(wire_w *w, uint64_t v);
void wire_i32(wire_w *w, int32_t v);
void wire_f32(wire_w *w, float v);
void wire_f64(wire_w *w, double v);
void wire_bytes(wire_w *w, const void *p, size_t n);
/* A string of at most max bytes, as a one-byte length and the bytes. */
void wire_str(wire_w *w, const char *s, size_t max);

uint8_t wire_get_u8(wire_r *r);
uint16_t wire_get_u16(wire_r *r);
uint32_t wire_get_u32(wire_r *r);
uint64_t wire_get_u64(wire_r *r);
int32_t wire_get_i32(wire_r *r);
float wire_get_f32(wire_r *r);  /* finite only */
double wire_get_f64(wire_r *r); /* finite only */
void wire_get_bytes(wire_r *r, void *out, size_t n);
/* A string written by wire_str into out (cap includes the NUL). Refused
 * if longer than cap - 1 or if it holds a byte below 0x20 or 0x7f; bytes
 * from 0x80 up are allowed (UTF-8), and checked by the caller if needed. */
void wire_get_str(wire_r *r, char *out, size_t cap);
/* Integer in [lo, hi], or bad. */
int32_t wire_get_range(wire_r *r, int32_t lo, int32_t hi);

/* One field of a struct for the tables below. */
typedef enum { WF_F32, WF_F64, WF_I32, WF_U32, WF_U8 } wire_type;
typedef struct {
    uint32_t off;  /* offsetof the field */
    uint8_t type;  /* wire_type */
    int32_t lo, hi; /* allowed range for WF_I32 and WF_U8 (WF_U32 and floats take any value) */
} wire_field;

/* Field initialisers: any value, or a range for fields used as an index
 * or an enum. */
#define WIRE_F32(T, f) {(uint32_t)offsetof(T, f), WF_F32, 0, 0}
#define WIRE_F64(T, f) {(uint32_t)offsetof(T, f), WF_F64, 0, 0}
#define WIRE_I32(T, f) {(uint32_t)offsetof(T, f), WF_I32, INT32_MIN, INT32_MAX}
#define WIRE_I32R(T, f, lo, hi) {(uint32_t)offsetof(T, f), WF_I32, (lo), (hi)}
#define WIRE_U32(T, f) {(uint32_t)offsetof(T, f), WF_U32, 0, 0}
#define WIRE_U8(T, f) {(uint32_t)offsetof(T, f), WF_U8, 0, 255}
#define WIRE_U8R(T, f, lo, hi) {(uint32_t)offsetof(T, f), WF_U8, (lo), (hi)}

void wire_put_fields(wire_w *w, const void *s, const wire_field *f, size_t n);
void wire_get_fields(wire_r *r, void *s, const wire_field *f, size_t n);

#endif
