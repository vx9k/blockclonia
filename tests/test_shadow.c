/* The shadow map's GPU-free parts (shadow.c): following the light, the
 * matrix and its fixed texel grid, which sections reach the map, which
 * faces it holds, and Auto's budget. */
#include "bloom.h"
#include "shadow.h"
#include "test_util.h"

#include <math.h>

/* Clip coordinates of camera-relative point p. */
static void apply(const float m[16], const float p[3], float out[3])
{
    for (int r = 0; r < 3; r++) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
}

static void test_aim(void)
{
    float kept[3] = {0.0f, 0.0f, 0.0f};
    /* Twilight (no contrast) and a light on the horizon cast nothing. */
    const float dusk[3] = {0.0f, 0.0f, 0.0f}, flat[3] = {0.5f, 0.01f, 0.0f};
    CHECK(!shadow_aim(kept, dusk) && !shadow_aim(kept, flat) && kept[0] == 0.0f);
    /* Noon, at half contrast: the unit direction. */
    const float noon[3] = {0.1f, 0.4f, 0.2f};
    CHECK(shadow_aim(kept, noon));
    float len = sqrtf(0.01f + 0.16f + 0.04f);
    CHECK(fabsf(kept[0] - 0.1f / len) < 1e-5f && fabsf(kept[1] - 0.4f / len) < 1e-5f);
    /* A tenth of a degree later the map holds still; a degree later it turns. */
    float a = 0.0017f, b = 0.0175f;
    const float later[3] = {noon[0] * cosf(a) - noon[1] * sinf(a), noon[0] * sinf(a) + noon[1] * cosf(a), noon[2]};
    float was = kept[0];
    CHECK(shadow_aim(kept, later) && kept[0] == was);
    const float more[3] = {noon[0] * cosf(b) - noon[1] * sinf(b), noon[0] * sinf(b) + noon[1] * cosf(b), noon[2]};
    CHECK(shadow_aim(kept, more) && kept[0] != was);
    /* The moon: the vector points at it, contrast and all. */
    const float moon[3] = {-0.05f, 0.3f, 0.0f};
    CHECK(shadow_aim(kept, moon) && kept[1] > 0.98f);
}

static void test_matrix(void)
{
    const float d[3] = {0.48f, 0.8f, 0.36f};
    float m[16] = {0}, c[3] = {0};
    dvec3 eye = dv3(1000.3, 70.0, -2000.7);
    shadow_matrix(d, eye, m);
    /* The eye is at the map's middle, give or take a texel, halfway in depth. */
    const float o[3] = {0.0f, 0.0f, 0.0f};
    apply(m, o, c);
    float texel = 2.0f / SHADOW_SIZE;
    CHECK(fabsf(c[0]) <= texel && fabsf(c[1]) <= texel && fabsf(c[2] - 0.5f) < 1e-6f);
    /* Toward the light is nearer (smaller z); across the map spans -1..1. */
    const float up[3] = {d[0] * 100.0f, d[1] * 100.0f, d[2] * 100.0f};
    float cu[3] = {0};
    apply(m, up, cu);
    CHECK(cu[2] < c[2] && fabsf(cu[0] - c[0]) < 1e-4f && fabsf(cu[1] - c[1]) < 1e-4f);
    CHECK(fabsf(cu[2] - (0.5f - 100.0f / (2.0f * SHADOW_DEPTH))) < 1e-5f);
    /* An edge of the map is SHADOW_RADIUS out, square to the light. */
    float side[3] = {d[1], -d[0], 0.0f}, sl = sqrtf(d[0] * d[0] + d[1] * d[1]);
    for (int i = 0; i < 3; i++) side[i] *= SHADOW_RADIUS / sl;
    float cs[3] = {0};
    apply(m, side, cs);
    CHECK(fabsf(sqrtf((cs[0] - c[0]) * (cs[0] - c[0]) + (cs[1] - c[1]) * (cs[1] - c[1])) - 1.0f) < 1e-4f);

    /* A fixed world point keeps its place on the texel grid as the eye
     * moves: its texel coordinate changes by whole texels only. */
    dvec3 w = dv3(1010.0, 75.0, -1990.0);
    float t0[2] = {0};
    for (int k = 0; k < 40; k++) {
        dvec3 e = dv3(eye.x + 0.137 * k, eye.y - 0.05 * k, eye.z + 0.291 * k);
        shadow_matrix(d, e, m);
        const float rel[3] = {(float)(w.x - e.x), (float)(w.y - e.y), (float)(w.z - e.z)};
        float cw[3] = {0};
        apply(m, rel, cw);
        float tx = cw[0] / texel, ty = cw[1] / texel;
        if (k == 0) {
            t0[0] = tx;
            t0[1] = ty;
            continue;
        }
        float fx = (tx - t0[0]) - roundf(tx - t0[0]), fy = (ty - t0[1]) - roundf(ty - t0[1]);
        CHECK(fabsf(fx) < 0.01f && fabsf(fy) < 0.01f);
    }
    /* Overhead light (not on the sun's path, but safe). */
    const float top[3] = {0.0f, 0.0f, 1.0f};
    shadow_matrix(top, eye, m);
    for (int i = 0; i < 16; i++) CHECK(isfinite(m[i]));
}

static void test_box(void)
{
    const float d[3] = {0.6f, 0.8f, 0.0f};
    float m[16] = {0};
    shadow_matrix(d, dv3(0.0, 64.0, 0.0), m);
    const float here[3] = {-8.0f, -8.0f, -8.0f}, far[3] = {300.0f, -8.0f, 300.0f}, beside[3] = {0.0f, 0.0f, 70.0f};
    CHECK(shadow_box(m, here, 16.0f));
    CHECK(!shadow_box(m, far, 16.0f));
    CHECK(!shadow_box(m, beside, 16.0f));                  /* past the edge across the light */
    const float edge[3] = {0.0f, 0.0f, 50.0f};
    CHECK(shadow_box(m, edge, 16.0f));                     /* straddles it */
    const float sunward[3] = {120.0f, 160.0f, 0.0f};       /* along the light: in the map, nearer */
    CHECK(shadow_box(m, sunward, 16.0f));
    const float beyond[3] = {600.0f, 800.0f, 0.0f};        /* past the depth range */
    CHECK(!shadow_box(m, beyond, 16.0f));
}

static void test_groups(void)
{
    const float d[3] = {0.6f, 0.8f, -0.1f};
    uint32_t g = shadow_groups(d);
    /* -X, -Y and +Z look away from a light up, +x and -z. */
    CHECK(g == (1u << 1 | 1u << 3 | 1u << 4));
    const float level[3] = {0.0f, 1.0f, 0.0f};
    CHECK(shadow_groups(level) == 1u << 3); /* only the bottoms */
}

/* Auto decides on its own budget. */
static void test_auto(void)
{
    bloom_auto a = {0};
    bloom_auto_reset(&a);
    int decided = 0;
    for (int i = 0; i < BLOOM_WARMUP + BLOOM_SAMPLES; i++) decided |= bloom_auto_add_within(&a, 0.4f, SHADOW_BUDGET_MS);
    CHECK(decided && a.on && bloom_active(SHADOW_AUTO, &a, 1));
    bloom_auto_reset(&a);
    for (int i = 0; i < BLOOM_WARMUP + BLOOM_SAMPLES; i++) bloom_auto_add_within(&a, 0.8f, SHADOW_BUDGET_MS);
    CHECK(a.decided && !a.on && !bloom_active(SHADOW_AUTO, &a, 1) && bloom_active(SHADOW_ON, &a, 1));
    /* Bloom's own budget is unchanged: 0.4 ms is too much for it. */
    bloom_auto_reset(&a);
    for (int i = 0; i < BLOOM_WARMUP + BLOOM_SAMPLES; i++) bloom_auto_add(&a, 0.4f);
    CHECK(a.decided && !a.on);
}

/* The casters' reach, in columns, covers the map's radius and the column
 * the eye stands in, and with the light overhead a section past it is off
 * the map. */
static void test_reach(void)
{
    CHECK(SHADOW_REACH == (int)(2.0f * SHADOW_RADIUS) / 16 + 1);
    CHECK((float)(SHADOW_REACH * 16) >= SHADOW_RADIUS + 16.0f);
    const float down[3] = {0.0f, 1.0f, 0.0f};
    float m[16] = {0};
    shadow_matrix(down, dv3(0.5, 70.0, 0.5), m);
    const float past[3] = {(float)(SHADOW_REACH * 16) + 16.0f, -8.0f, 0.0f};
    const float near[3] = {32.0f, -8.0f, 0.0f};
    CHECK(!shadow_box(m, past, 16.0f));
    CHECK(shadow_box(m, near, 16.0f));
}

void test_shadow_all(void)
{
    test_reach();
    test_aim();
    test_matrix();
    test_box();
    test_groups();
    test_auto();
}
