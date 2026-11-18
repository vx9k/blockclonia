/* Bloom's GPU-free parts: the glowing layers, the blur kernel and the
 * Auto setting's decision. */
#include "block.h"
#include "bloom.h"
#include "test_util.h"

#include <math.h>

static void test_mask(void)
{
    uint32_t m[BLOOM_MASK_WORDS] = {0};
    bloom_glow_mask(m);
    CHECK(bloom_layer_glows(m, T_LAVA));
    for (uint32_t l = T_FIRE0; l <= T_FIRE3; l++) CHECK(bloom_layer_glows(m, l));
    CHECK(bloom_layer_glows(m, T_CAMPFIRE_BASE));
    CHECK(bloom_layer_glows(m, T_FOXFIRE));
    CHECK(bloom_layer_glows(m, T_LAMP));
    CHECK(bloom_layer_glows(m, T_FLASH)); /* a muzzle flash or tracer */
    CHECK(!bloom_layer_glows(m, T_SMOKE0) && !bloom_layer_glows(m, T_SMOKE2));
    /* See-through but not alight: water, glass, oil, ice. */
    CHECK(!bloom_layer_glows(m, T_WATER));
    CHECK(!bloom_layer_glows(m, T_GLASS));
    CHECK(!bloom_layer_glows(m, T_CRUDE_OIL));
    CHECK(!bloom_layer_glows(m, T_ICE));
    CHECK(!bloom_layer_glows(m, T_STONE));
    CHECK(!bloom_layer_glows(m, 300u));
    int n = 0;
    for (uint32_t l = 0; l < T_COUNT; l++) n += bloom_layer_glows(m, l);
    CHECK(n == 9);
}

static uint32_t quad_vertex(uint32_t layer) { return layer << 20 | 3u << 15; }

static void test_quads(void)
{
    uint32_t m[BLOOM_MASK_WORDS] = {0};
    bloom_glow_mask(m);
    uint32_t v[12] = {0};
    for (int i = 0; i < 12; i++) v[i] = quad_vertex(i < 8 ? T_WATER : T_GLASS);
    CHECK(!bloom_quads_glow(v, 3, m));
    v[8] = v[9] = v[10] = v[11] = quad_vertex(T_LAVA);
    CHECK(bloom_quads_glow(v, 3, m));
    /* Only the quads asked about count. */
    CHECK(!bloom_quads_glow(v, 2, m));
    CHECK(!bloom_quads_glow(v, 0, m));
}

static void test_kernel(void)
{
    float w[BLOOM_TAPS] = {0}, o[BLOOM_TAPS] = {0};
    bloom_kernel(3.0f, w, o);
    float sum = w[0];
    for (int t = 1; t < BLOOM_TAPS; t++) sum += 2.0f * w[t];
    CHECK(fabsf(sum - 1.0f) < 1e-5f);
    CHECK(o[0] == 0.0f);
    for (int t = 1; t < BLOOM_TAPS; t++) {
        /* Each fetch lies between its two texels, nearer the heavier inner one. */
        CHECK(o[t] > (float)(2 * t - 1) && o[t] < (float)(2 * t - 1) + 0.5f);
        CHECK(w[t] < w[t - 1] || t == 1);
    }
    /* The pair (1, 2) carries g(1) + g(2) of the Gaussian. */
    float g0 = 1.0f, g1 = expf(-1.0f / 18.0f), g2 = expf(-4.0f / 18.0f);
    CHECK(fabsf(w[1] / w[0] - (g1 + g2) / g0) < 1e-4f);
    CHECK(fabsf(o[1] - (1.0f + g2 / (g1 + g2))) < 1e-4f);
    /* A narrower blur keeps more in the centre. */
    float wn[BLOOM_TAPS] = {0}, on[BLOOM_TAPS] = {0};
    bloom_kernel(1.5f, wn, on);
    CHECK(wn[0] > w[0]);
}

static void feed(bloom_auto *a, int frames, float ms)
{
    for (int i = 0; i < frames; i++) (void)bloom_auto_add(a, ms);
}

static void test_auto(void)
{
    bloom_auto a = {0};
    bloom_auto_reset(&a);
    /* On while it measures; off without a way to measure. */
    CHECK(bloom_active(BLOOM_AUTO, &a, 1));
    CHECK(!bloom_active(BLOOM_AUTO, &a, 0));
    CHECK(bloom_active(BLOOM_ON, &a, 0));
    CHECK(!bloom_active(BLOOM_OFF, &a, 1));
    /* The warm-up is ignored, however slow. */
    feed(&a, BLOOM_WARMUP, 50.0f);
    CHECK(a.count == 0 && !a.decided);
    feed(&a, BLOOM_SAMPLES - 1, 0.2f);
    CHECK(!a.decided);
    CHECK(bloom_auto_add(&a, 0.2f) == 1);
    CHECK(a.decided && a.on);
    CHECK(fabsf(a.median - 0.2f) < 1e-6f);
    CHECK(bloom_active(BLOOM_AUTO, &a, 1));
    /* Decided once: later frames change nothing. */
    CHECK(bloom_auto_add(&a, 9.0f) == 0);
    CHECK(a.on);

    /* Too slow: off, and Off and On still override it. */
    bloom_auto_reset(&a);
    feed(&a, BLOOM_WARMUP + BLOOM_SAMPLES, 0.45f);
    CHECK(a.decided && !a.on);
    CHECK(!bloom_active(BLOOM_AUTO, &a, 1));
    CHECK(bloom_active(BLOOM_ON, &a, 1));

    /* A few stalls don't decide it: the median does. */
    bloom_auto_reset(&a);
    feed(&a, BLOOM_WARMUP, 0.0f);
    for (int i = 0; i < BLOOM_SAMPLES; i++) (void)bloom_auto_add(&a, i % 4 == 0 ? 5.0f : 0.25f);
    CHECK(a.decided && a.on);
    /* And most frames over budget turn it off even with fast ones mixed in. */
    bloom_auto_reset(&a);
    feed(&a, BLOOM_WARMUP, 0.0f);
    for (int i = 0; i < BLOOM_SAMPLES; i++) (void)bloom_auto_add(&a, i % 3 == 0 ? 0.1f : 0.31f);
    CHECK(a.decided && !a.on);
}

void test_bloom_all(void)
{
    test_mask();
    test_quads();
    test_kernel();
    test_auto();
}
