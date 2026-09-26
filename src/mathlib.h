/* Small header-only math library. Matrices are column-major (GLSL layout). */
#ifndef MC_MATHLIB_H
#define MC_MATHLIB_H

#include <math.h>
#include <stdint.h>

#define MC_PI 3.14159265358979323846

typedef struct { float x, y, z; } vec3;
typedef struct { double x, y, z; } dvec3;
typedef struct { float m[16]; } mat4;

static inline vec3 v3(float x, float y, float z) { return (vec3){x, y, z}; }
static inline vec3 v3_scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float v3_len(vec3 a) { return sqrtf(v3_dot(a, a)); }
static inline vec3 v3_cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline vec3 v3_norm(vec3 a)
{
    float l = v3_len(a);
    return l > 1e-12f ? v3_scale(a, 1.0f / l) : a;
}

static inline dvec3 dv3(double x, double y, double z) { return (dvec3){x, y, z}; }
static inline dvec3 dv3_add(dvec3 a, dvec3 b) { return dv3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline dvec3 dv3_scale(dvec3 a, double s) { return dv3(a.x * s, a.y * s, a.z * s); }
static inline double dv3_len(dvec3 a) { return sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
static inline dvec3 dv3_lerp(dvec3 a, dvec3 b, double t)
{
    return dv3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline mat4 m4_identity(void)
{
    mat4 r = {{0}};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static inline mat4 m4_mul(mat4 a, mat4 b)
{
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

/* Reversed-Z, infinite far plane perspective for Vulkan clip space
 * (y down, depth 0..1). Near plane maps to depth 1, infinity to 0. This
 * gives near-uniform depth precision with a float depth buffer. */
static inline mat4 m4_perspective_revz(float fovy, float aspect, float znear)
{
    float f = 1.0f / tanf(fovy * 0.5f);
    mat4 r = {{0}};
    r.m[0] = f / aspect;
    r.m[5] = -f;
    r.m[11] = -1.0f;
    r.m[14] = znear;
    return r;
}

/* Rotation-only view matrix (the camera sits at the origin; the world is
 * rendered camera-relative to keep float precision far from spawn). */
static inline mat4 m4_view_rot(float yaw, float pitch)
{
    vec3 fwd = v3(cosf(pitch) * sinf(yaw), sinf(pitch), -cosf(pitch) * cosf(yaw));
    vec3 right = v3_norm(v3_cross(fwd, v3(0, 1, 0)));
    vec3 up = v3_cross(right, fwd);
    mat4 r = m4_identity();
    r.m[0] = right.x; r.m[4] = right.y; r.m[8] = right.z;
    r.m[1] = up.x;    r.m[5] = up.y;    r.m[9] = up.z;
    r.m[2] = -fwd.x;  r.m[6] = -fwd.y;  r.m[10] = -fwd.z;
    return r;
}

/* The same with the camera rolled about its view axis (head tilt). */
static inline mat4 m4_view_rot_roll(float yaw, float pitch, float roll)
{
    vec3 fwd = v3(cosf(pitch) * sinf(yaw), sinf(pitch), -cosf(pitch) * cosf(yaw));
    vec3 right = v3_norm(v3_cross(fwd, v3(0, 1, 0)));
    vec3 up = v3_cross(right, fwd);
    float c = cosf(roll), s = sinf(roll);
    vec3 r2 = v3(right.x * c + up.x * s, right.y * c + up.y * s, right.z * c + up.z * s);
    vec3 u2 = v3(up.x * c - right.x * s, up.y * c - right.y * s, up.z * c - right.z * s);
    mat4 r = m4_identity();
    r.m[0] = r2.x; r.m[4] = r2.y; r.m[8] = r2.z;
    r.m[1] = u2.x; r.m[5] = u2.y; r.m[9] = u2.z;
    r.m[2] = -fwd.x; r.m[6] = -fwd.y; r.m[10] = -fwd.z;
    return r;
}

static inline vec3 look_dir(float yaw, float pitch)
{
    return v3(cosf(pitch) * sinf(yaw), sinf(pitch), -cosf(pitch) * cosf(yaw));
}

/* Frustum as 4 side planes (a,b,c,d): dot(n,p)+d >= 0 is inside. The far
 * plane is at infinity and fog hides the render-distance edge, so near/far
 * planes are not tested. */
typedef struct { float p[4][4]; } frustum;

static inline frustum frustum_from(mat4 vp)
{
    frustum f;
    const float *m = vp.m;
    for (int i = 0; i < 4; i++) {
        float r3 = m[i * 4 + 3], r0 = m[i * 4 + 0], r1 = m[i * 4 + 1];
        f.p[0][i] = r3 + r0;
        f.p[1][i] = r3 - r0;
        f.p[2][i] = r3 + r1;
        f.p[3][i] = r3 - r1;
    }
    for (int k = 0; k < 4; k++) {
        float l = sqrtf(f.p[k][0] * f.p[k][0] + f.p[k][1] * f.p[k][1] + f.p[k][2] * f.p[k][2]);
        if (l > 0) for (int i = 0; i < 4; i++) f.p[k][i] /= l;
    }
    return f;
}

/* Box given by min corner and size; returns 0 when fully outside. */
static inline int frustum_box(const frustum *f, vec3 mn, vec3 size)
{
    for (int k = 0; k < 4; k++) {
        const float *p = f->p[k];
        vec3 v = v3(mn.x + (p[0] >= 0 ? size.x : 0), mn.y + (p[1] >= 0 ? size.y : 0),
                    mn.z + (p[2] >= 0 ? size.z : 0));
        if (p[0] * v.x + p[1] * v.y + p[2] * v.z + p[3] < 0) return 0;
    }
    return 1;
}

#endif
