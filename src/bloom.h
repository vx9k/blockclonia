/* Bloom: the glow around fire, lava, torches, foxfire and the moon. The
 * renderer marks glowing pixels in the scene image's alpha, blurs them at
 * a quarter of the resolution and adds the result back. This module holds
 * the parts that need no GPU: which texture layers glow, the blur's
 * weights, and the Auto setting's decision, which keeps bloom only where
 * the GPU's own timestamps show it costs under BLOOM_BUDGET_MS a frame.
 * It doesn't draw anything or read the clock. */
#ifndef MC_BLOOM_H
#define MC_BLOOM_H

#include <stdint.h>

/* The `bloom` setting. */
#define BLOOM_OFF 0
#define BLOOM_ON 1
#define BLOOM_AUTO 2

/* Auto keeps bloom when its median cost is under this. 0.3 ms is about 2%
 * of a 60 Hz frame on the Intel N-series iGPU the shaders are tuned on. */
#define BLOOM_BUDGET_MS 0.3f
/* Frames skipped after a start or resize (pipelines warming up, the
 * driver raising clocks), then frames measured. */
#define BLOOM_WARMUP 30
#define BLOOM_SAMPLES 31

/* The blur's standard deviation in quarter-resolution texels (12 pixels
 * of the full image), and how much of the blurred glow is added. */
#define BLOOM_SIGMA 3.0f
#define BLOOM_STRENGTH 0.8f

/* One bit per texture layer (block.h's T_* order), as the fragment
 * shader's specialization constants. */
#define BLOOM_MASK_WORDS 8

/* Blur taps: the centre and four bilinear pairs on each side, nine
 * fetches standing for a 17-texel Gaussian. */
#define BLOOM_TAPS 5

typedef struct {
    int frames;              /* frames seen since the reset */
    int count;               /* samples kept */
    float ms[BLOOM_SAMPLES]; /* bloom cost per measured frame */
    int decided, on;
    float median; /* once decided */
} bloom_auto;

void bloom_auto_reset(bloom_auto *a);
/* One frame's bloom cost in ms. Ignored during the warm-up and once
 * decided; the last sample decides. Returns 1 on the frame it decides. */
int bloom_auto_add(bloom_auto *a, float ms);
/* Whether to draw bloom this frame. Auto measures with bloom on, and
 * without GPU timestamps (can_measure 0) it cannot, so it stays off. */
int bloom_active(int mode, const bloom_auto *a, int can_measure);

/* Sets the bit of every layer whose bright texels glow. */
void bloom_glow_mask(uint32_t mask[BLOOM_MASK_WORDS]);
int bloom_layer_glows(const uint32_t mask[BLOOM_MASK_WORDS], uint32_t layer);
/* 1 if any of the quads (mesher.h vertices, four per quad) has a glowing
 * layer: such a section needs the glow pass. */
int bloom_quads_glow(const uint32_t *verts, uint32_t quads, const uint32_t mask[BLOOM_MASK_WORDS]);

/* A Gaussian of standard deviation sigma texels over 17 texels, folded
 * into linear-filtered taps: weight[0] at offset 0, then weight[i] at
 * +-offset[i]. The weights sum to 1. */
void bloom_kernel(float sigma, float weight[BLOOM_TAPS], float offset[BLOOM_TAPS]);

#endif
