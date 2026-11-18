#include "shadow.h"

#include <math.h>

int shadow_aim(float kept[3], const float sun[3])
{
    float len = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    /* Below 2% contrast the shading a shadow takes away is under a
     * level step; and a light at the horizon would stretch the map. */
    if (len < 0.02f || sun[1] / len < 0.05f) return 0;
    const float d[3] = {sun[0] / len, sun[1] / len, sun[2] / len};
    float dot = d[0] * kept[0] + d[1] * kept[1] + d[2] * kept[2];
    if (dot < cosf(SHADOW_TURN))
        for (int i = 0; i < 3; i++) kept[i] = d[i];
    return 1;
}

/* Where along axis a the eye is, less the texel line below it: what the
 * map shifts by so its texels stay put in the world. In double, as world
 * positions are. */
static float snap(const float a[3], dvec3 eye)
{
    double texel = 2.0 * (double)SHADOW_RADIUS / SHADOW_SIZE;
    double along = (double)a[0] * eye.x + (double)a[1] * eye.y + (double)a[2] * eye.z;
    return (float)(along - floor(along / texel) * texel);
}

void shadow_matrix(const float dir[3], dvec3 eye, float m[16])
{
    /* u, v across the map, d toward the light. Up is a poor reference only
     * with the light overhead, which the tilted sun path never reaches. */
    const float *d = dir;
    float ref[3] = {0.0f, 0.0f, 1.0f};
    if (fabsf(d[2]) > 0.9f) {
        ref[1] = 1.0f;
        ref[2] = 0.0f;
    }
    float u[3] = {ref[1] * d[2] - ref[2] * d[1], ref[2] * d[0] - ref[0] * d[2], ref[0] * d[1] - ref[1] * d[0]};
    float ul = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int i = 0; i < 3; i++) u[i] /= ul;
    const float v[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};
    float su = snap(u, eye), sv = snap(v, eye);
    for (int c = 0; c < 3; c++) {
        m[c * 4 + 0] = u[c] / SHADOW_RADIUS;
        m[c * 4 + 1] = v[c] / SHADOW_RADIUS;
        m[c * 4 + 2] = -d[c] / (2.0f * SHADOW_DEPTH);
        m[c * 4 + 3] = 0.0f;
    }
    m[12] = su / SHADOW_RADIUS;
    m[13] = sv / SHADOW_RADIUS;
    m[14] = 0.5f;
    m[15] = 1.0f;
}

int shadow_box(const float m[16], const float lo[3], float size)
{
    float h = 0.5f * size;
    for (int row = 0; row < 3; row++) {
        float c = m[12 + row], r = 0.0f;
        for (int k = 0; k < 3; k++) {
            c += m[k * 4 + row] * (lo[k] + h);
            r += fabsf(m[k * 4 + row]) * h;
        }
        float lo_edge = row < 2 ? -1.0f : 0.0f;
        if (c + r < lo_edge || c - r > 1.0f) return 0;
    }
    return 1;
}

uint32_t shadow_groups(const float dir[3])
{
    uint32_t g = 0;
    for (int a = 0; a < 3; a++) {
        if (dir[a] < 0.0f) g |= 1u << (2 * a);     /* the + face looks away */
        if (dir[a] > 0.0f) g |= 1u << (2 * a + 1); /* the - face does */
    }
    return g;
}
