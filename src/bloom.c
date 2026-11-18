#include "bloom.h"

#include "block.h"

#include <math.h>
#include <string.h>

_Static_assert(T_COUNT <= BLOOM_MASK_WORDS * 32, "a glow bit per texture layer");

/* What gives off its own light: the melt, the flames, the embers under a
 * campfire's logs, foxfire's threads, the lamp's flame, and a muzzle flash
 * or tracer. The shader
 * lets only a layer's bright texels glow, so lava's cooled skin and the
 * logs stay dark. */
static const int GLOWING[] = {T_LAVA, T_FIRE0, T_FIRE1, T_FIRE2, T_FIRE3, T_CAMPFIRE_BASE, T_FOXFIRE, T_LAMP, T_FLASH};

void bloom_auto_reset(bloom_auto *a) { memset(a, 0, sizeof *a); }

int bloom_auto_add(bloom_auto *a, float ms)
{
    if (a->decided) return 0;
    if (a->frames++ < BLOOM_WARMUP) return 0;
    a->ms[a->count++] = ms;
    if (a->count < BLOOM_SAMPLES) return 0;
    /* The median: a frame that stalls on a page fault or a clock change
     * must not decide it either way. */
    float s[BLOOM_SAMPLES] = {0};
    memcpy(s, a->ms, sizeof s);
    for (int i = 1; i < BLOOM_SAMPLES; i++)
        for (int j = i; j > 0 && s[j - 1] > s[j]; j--) {
            float t = s[j];
            s[j] = s[j - 1];
            s[j - 1] = t;
        }
    a->median = s[BLOOM_SAMPLES / 2];
    a->on = a->median < BLOOM_BUDGET_MS;
    a->decided = 1;
    return 1;
}

int bloom_active(int mode, const bloom_auto *a, int can_measure)
{
    if (mode == BLOOM_ON) return 1;
    if (mode != BLOOM_AUTO || !can_measure) return 0;
    return a->decided ? a->on : 1;
}

void bloom_glow_mask(uint32_t mask[BLOOM_MASK_WORDS])
{
    memset(mask, 0, BLOOM_MASK_WORDS * sizeof mask[0]);
    for (size_t i = 0; i < sizeof GLOWING / sizeof GLOWING[0]; i++)
        mask[(unsigned)GLOWING[i] >> 5] |= 1u << ((unsigned)GLOWING[i] & 31u);
}

int bloom_layer_glows(const uint32_t mask[BLOOM_MASK_WORDS], uint32_t layer)
{
    return layer < BLOOM_MASK_WORDS * 32u && (mask[layer >> 5] >> (layer & 31u) & 1u);
}

int bloom_quads_glow(const uint32_t *verts, uint32_t quads, const uint32_t mask[BLOOM_MASK_WORDS])
{
    /* A quad's four vertices share its layer (bits 20..27). */
    for (uint32_t q = 0; q < quads; q++)
        if (bloom_layer_glows(mask, verts[q * 4u] >> 20 & 255u)) return 1;
    return 0;
}

void bloom_kernel(float sigma, float weight[BLOOM_TAPS], float offset[BLOOM_TAPS])
{
    /* Texels 0..8 on each side. A bilinear fetch between texels a and a+1
     * at a + w1 / (w0 + w1) returns their weighted sum, so each pair costs
     * one fetch. */
    float g[2 * (BLOOM_TAPS - 1) + 1] = {0};
    float sum = 0.0f;
    for (int i = 0; i < 2 * (BLOOM_TAPS - 1) + 1; i++) {
        g[i] = expf(-(float)(i * i) / (2.0f * sigma * sigma));
        sum += i ? 2.0f * g[i] : g[i];
    }
    weight[0] = g[0] / sum;
    offset[0] = 0.0f;
    for (int t = 1; t < BLOOM_TAPS; t++) {
        float w0 = g[2 * t - 1], w1 = g[2 * t];
        weight[t] = (w0 + w1) / sum;
        offset[t] = (float)(2 * t - 1) + w1 / (w0 + w1);
    }
}
