/* The renderer's instanced-cube format, shared by everything that draws
 * entities (dropped items, particles, overlays, the view model), and the
 * quaternion helpers that orient them. */
#ifndef MC_ENTITY_H
#define MC_ENTITY_H

#include <math.h>
#include <stdint.h>

typedef struct {
    float pos[3];        /* centre relative to the camera (view space for the view model) */
    uint32_t tex;        /* texture layers: side | top << 8 | bottom << 16 | front << 24 | ENTITY_TEX_METRIC */
    float rot[4];        /* unit quaternion x, y, z, w */
    float scale[3];      /* size in metres along the cube's own axes */
    float light;         /* brightness multiplier */
    uint32_t tint;       /* colour multiplied into the texture, stored as white minus it so that 0
                            (any zeroed instance) is no tint: entity_tint */
} entity_instance;

/* The tint field for colour 0xRRGGBB. */
static inline uint32_t entity_tint(uint32_t rgb)
{
    uint32_t c = ~rgb;
    return (c & 0xFF0000u) >> 16 | (c & 0xFF00u) | (c & 0xFFu) << 16;
}

/* Pixel art drawn to a box's size, as the creatures are. Without it every
 * face stretches its whole 16-texel layer, which suits items and blocks.
 * With it a face shows the top-left (size in metres x 16) texels of its
 * layer, so texels are 1/16 m on every box, and art is drawn with its
 * first column at the front (-z) and its first row at the top. Boxes stay
 * within 1 m along each axis, the size of one layer. */
#define ENTITY_TEX_METRIC 0x80000000u
#define ENTITY_TEX_FRONT_MAX 127 /* the front layer has 7 bits; 0 = the side layer */

/* Layers for a box: sides, top, bottom, and the front (-z) face, which
 * takes the side layer when front is 0. */
static inline uint32_t entity_tex(int side, int top, int bottom, int front)
{
    uint32_t faces = (uint32_t)side | (uint32_t)top << 8 | (uint32_t)bottom << 16;
    return faces | (uint32_t)front << 24;
}

typedef struct {
    float x, y, z, w;
} quat;

static inline quat quat_identity(void) { return (quat){0.0f, 0.0f, 0.0f, 1.0f}; }

static inline quat quat_axis(float ax, float ay, float az, float angle)
{
    float l = sqrtf(ax * ax + ay * ay + az * az);
    if (l < 1e-9f) return quat_identity();
    float s = sinf(angle * 0.5f) / l;
    return (quat){ax * s, ay * s, az * s, cosf(angle * 0.5f)};
}

/* a then b: rotating by the result is rotating by b, then by a. */
static inline quat quat_mul(quat a, quat b)
{
    return (quat){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                  a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static inline quat quat_norm(quat q)
{
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-9f) return quat_identity();
    return (quat){q.x / l, q.y / l, q.z / l, q.w / l};
}

/* Rotates v by q. */
static inline void quat_apply(quat q, const float v[3], float out[3])
{
    float cx = q.y * v[2] - q.z * v[1] + q.w * v[0];
    float cy = q.z * v[0] - q.x * v[2] + q.w * v[1];
    float cz = q.x * v[1] - q.y * v[0] + q.w * v[2];
    out[0] = v[0] + 2.0f * (q.y * cz - q.z * cy);
    out[1] = v[1] + 2.0f * (q.z * cx - q.x * cz);
    out[2] = v[2] + 2.0f * (q.x * cy - q.y * cx);
}

static inline void entity_set_rot(entity_instance *e, quat q)
{
    e->rot[0] = q.x;
    e->rot[1] = q.y;
    e->rot[2] = q.z;
    e->rot[3] = q.w;
}

#endif
