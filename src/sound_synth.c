#include "sound_synth.h"
#include "block.h"
#include "mem.h"
#include "sound.h"

#include <math.h>
#include <string.h>
#include <time.h>

/* The building blocks, all in float at SYNTH_RATE:
 *
 * - white noise from a seeded xorshift generator, shaped by one-pole
 *   low/high passes and RBJ biquad band passes;
 * - modal synthesis for knocks and rings: a struck object rings at its
 *   natural frequencies, each a sinusoid decaying with its own time
 *   constant (wood: few low modes that die fast; glass: high, long ones);
 * - granular crunches for sand, gravel and snow: many tiny band-limited
 *   bursts at random times, their size and pitch set by the grain size;
 * - chirps (swept sines) for bubbles and pops, and a pulse train through
 *   two formant filters for the hurt grunt.
 *
 * Every clip gets a DC blocker, short fades so it never clicks, and is
 * normalised to full 16-bit scale; its playback gain lives in the clip.
 * Everything is deterministic for a seed. */

_Static_assert(SND_COUNT <= 32, "synth_bank.clip has room for 32 sound ids");

#define TAU 6.28318530717958647692f
#define FS ((float)SYNTH_RATE)
#define MAX_CLIP_S 3.6f
#define MAX_CLIP ((int)(MAX_CLIP_S * FS))

/* ---------------------------------------------------------- primitives */

typedef struct {
    uint32_t s;
} rng_t;

static float rnd(rng_t *r) /* 0..1 */
{
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return (float)(r->s >> 8) / 16777216.0f;
}

static float rnd_s(rng_t *r) { return rnd(r) * 2.0f - 1.0f; } /* -1..1 */

static int secs(float s) { return (int)(s * FS); }

/* Attack-decay envelope, (1 - e^(-t/a)) e^(-t/d), stepped by running
 * products instead of two exponentials a sample. */
typedef struct {
    float ea, ca, ed, cd; /* e^(-t/a) and e^(-t/d), and their per-sample factors */
} env;

static env env_start(float a, float d) { return (env){1.0f, expf(-1.0f / (a * FS)), 1.0f, expf(-1.0f / (d * FS))}; }

static float env_next(env *e)
{
    float v = (1.0f - e->ea) * e->ed;
    /* Flushed at 1e-20 before they go subnormal: decaying through the
     * subnormal range costs ~100x per multiply on most CPUs. */
    e->ea = e->ea > 1e-20f ? e->ea * e->ca : 0.0f;
    e->ed = e->ed > 1e-20f ? e->ed * e->cd : 0.0f;
    return v;
}

typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} biquad;

/* RBJ band pass, 0 dB at the centre. */
static biquad bandpass(float f, float q)
{
    float w = TAU * fminf(f, FS * 0.45f) / FS, al = sinf(w) / (2.0f * q), a0 = 1.0f + al;
    biquad b = {al / a0, 0.0f, -al / a0, -2.0f * cosf(w) / a0, (1.0f - al) / a0, 0.0f, 0.0f};
    return b;
}

static float bq_run(biquad *b, float x)
{
    float y = b->b0 * x + b->z1;
    b->z1 = b->b1 * x - b->a1 * y + b->z2;
    b->z2 = b->b2 * x - b->a2 * y;
    return y;
}

/* One-pole coefficient for a cutoff. */
static float pole(float f) { return 1.0f - expf(-TAU * f / FS); }

/* Noise shaped by a high pass (hp Hz, 0 none) and a two-pole low pass (lp
 * Hz), under an attack/decay envelope, added at `at`. */
static void add_noise(float *buf, int n, int at, int len, float amp, float hp, float lp, float a, float d, rng_t *r)
{
    float ch = hp > 0.0f ? pole(hp) : 0.0f, cl = pole(lp), h = 0.0f, l1 = 0.0f, l2 = 0.0f;
    env e = env_start(a, d);
    for (int i = 0; i < len && at + i < n; i++) {
        float x = rnd_s(r);
        h += ch * (x - h);
        x -= h; /* high pass: what the slow follower misses */
        l1 += cl * (x - l1);
        l2 += cl * (l1 - l2);
        buf[at + i] += amp * l2 * env_next(&e);
    }
}

/* Band-passed noise (centre f, quality q) under an envelope. */
static void add_band(float *buf, int n, int at, int len, float amp, float f, float q, float a, float d, rng_t *r)
{
    biquad b = bandpass(f, q);
    env e = env_start(a, d);
    for (int i = 0; i < len && at + i < n; i++) buf[at + i] += amp * bq_run(&b, rnd_s(r)) * env_next(&e);
}

/* Damped modes: frequency, decay (s), relative amplitude. */
typedef struct {
    float f, tau, amp;
} mode;

static void add_modes(float *buf, int n, int at, float amp, const mode *m, int count, float pitch, rng_t *r)
{
    for (int k = 0; k < count; k++) {
        float f = m[k].f * pitch * (1.0f + 0.03f * rnd_s(r)), w = TAU * f / FS, ph = rnd(r) * TAU;
        if (w >= 3.1f) continue; /* above Nyquist */
        int len = secs(m[k].tau * 7.0f);
        /* A decaying sinusoid by recursion: y[i] = 2 c cos(w) y[i-1] - c^2 y[i-2],
         * started on the exact first two samples. */
        float c = expf(-1.0f / (m[k].tau * FS)), g = amp * m[k].amp;
        float k1 = 2.0f * c * cosf(w), k2 = c * c;
        float y0 = g * sinf(ph), y1 = g * c * sinf(ph + w);
        for (int i = 0; i < len && at + i < n; i++) {
            buf[at + i] += y0;
            float y2 = k1 * y1 - k2 * y0;
            y0 = y1;
            y1 = y2;
        }
    }
}

/* Many tiny bursts spread over `span` samples: crunch and crumble. */
static void add_grains(float *buf, int n, int at, int span, int count, float grain_s, float f, float q, float amp,
                       rng_t *r)
{
    for (int g = 0; g < count; g++) {
        /* Grains bunch towards the start, like a foot settling. */
        float u = rnd(r);
        int start = at + (int)((float)span * u * u);
        float a = amp * (0.3f + 0.7f * rnd(r));
        biquad b = bandpass(f * (0.7f + 0.6f * rnd(r)), q);
        int len = secs(grain_s * (0.5f + rnd(r)));
        for (int i = 0; i < len && start + i < n; i++) {
            float w = sinf(3.14159265f * (float)i / (float)len); /* half-sine window */
            buf[start + i] += a * w * bq_run(&b, rnd_s(r));
        }
    }
}

/* A sine sweeping f0 -> f1, decaying with d: bubbles, pops, squeaks. */
static void add_chirp(float *buf, int n, int at, int len, float f0, float f1, float amp, float d)
{
    float ph = 0.0f;
    env e = env_start(0.002f, d);
    for (int i = 0; i < len && at + i < n; i++) {
        float t = (float)i / (float)len, f = f0 + (f1 - f0) * t;
        ph += TAU * f / FS;
        buf[at + i] += amp * sinf(ph) * env_next(&e);
    }
}

/* A low body thump: a sine that drops in pitch, plus a little noise. */
static void add_thump(float *buf, int n, int at, float f, float amp, float d, rng_t *r)
{
    int len = secs(d * 6.0f);
    float ph = 0.0f, drop = 1.0f, dc = expf(-1.0f / (0.015f * FS));
    env e = env_start(0.002f, d);
    for (int i = 0; i < len && at + i < n; i++) {
        ph += TAU * f * (1.0f + 0.6f * drop) / FS;
        drop = drop > 1e-20f ? drop * dc : 0.0f;
        buf[at + i] += amp * sinf(ph) * env_next(&e);
    }
    add_noise(buf, n, at, len, amp * 0.3f, 0.0f, f * 3.0f, 0.001f, d * 0.6f, r);
}

/* A very short broadband click: the contact of two hard things. */
static void add_click(float *buf, int n, int at, float amp, float bright, rng_t *r)
{ add_noise(buf, n, at, secs(0.006f), amp, 400.0f, 3000.0f + 9000.0f * bright, 0.0002f, 0.0012f, r); }

/* ------------------------------------------------------------ finishing */

/* DC blocker, fades, normalisation to full scale; appends to the bank. */
static synth_clip finish(synth_bank *b, float *buf, int n, float gain)
{
    synth_clip c = {NULL, 0, 0.0f};
    if (n <= 0 || b->used + (uint32_t)n > b->cap) return c;
    float x1 = 0.0f, y1 = 0.0f, peak = 1e-9f, rdc = 1.0f - TAU * 25.0f / FS;
    int fin = secs(0.0015f), fout = secs(0.006f);
    for (int i = 0; i < n; i++) {
        float y = buf[i] - x1 + rdc * y1;
        x1 = buf[i];
        y1 = y;
        if (i < fin) y *= (float)i / (float)fin;
        if (i >= n - fout) y *= (float)(n - 1 - i) / (float)fout;
        buf[i] = y;
        peak = fmaxf(peak, fabsf(y));
    }
    int16_t *dst = b->pcm + b->used;
    float k = 32000.0f / peak;
    for (int i = 0; i < n; i++) dst[i] = (int16_t)lrintf(buf[i] * k);
    b->used += (uint32_t)n;
    c.pcm = dst;
    c.len = (uint32_t)n;
    c.gain = gain;
    return c;
}

/* A loop: generated `n + xf` long, then the tail is crossfaded (equal
 * power) into the head so sample n-1 flows into sample 0. */
static synth_clip finish_loop(synth_bank *b, float *buf, int n, int xf, float gain)
{
    for (int i = 0; i < xf; i++) {
        float t = (float)i / (float)xf;
        buf[i] = buf[i] * sinf(t * 1.5707963f) + buf[n + i] * cosf(t * 1.5707963f);
    }
    synth_clip c = {NULL, 0, 0.0f};
    if (b->used + (uint32_t)n > b->cap) return c;
    float peak = 1e-9f, mean = 0.0f;
    for (int i = 0; i < n; i++) mean += buf[i];
    mean /= (float)n;
    for (int i = 0; i < n; i++) peak = fmaxf(peak, fabsf(buf[i] - mean));
    int16_t *dst = b->pcm + b->used;
    for (int i = 0; i < n; i++) dst[i] = (int16_t)lrintf((buf[i] - mean) * 32000.0f / peak);
    b->used += (uint32_t)n;
    c.pcm = dst;
    c.len = (uint32_t)n;
    c.gain = gain;
    return c;
}

/* ------------------------------------------------------------ materials */

synth_material synth_material_of(uint8_t block)
{
    switch (block) {
    case B_GRASS: return MAT_GRASS;
    case B_DIRT:
    case B_ASH: return MAT_DIRT;
    case B_STONE:
    case B_BEDROCK:
    case B_BRICK: return MAT_STONE;
    case B_LOG:
    case B_PLANKS:
    case B_CAMPFIRE: return MAT_WOOD;
    case B_SAND: return MAT_SAND;
    case B_GRAVEL: return MAT_GRAVEL;
    case B_SNOW: return MAT_SNOW;
    case B_LEAVES: return MAT_LEAVES;
    case B_GLASS:
    case B_ICE: return MAT_GLASS;
    case B_WATER: return MAT_WATER;
    default: return MAT_GENERIC;
    }
}

int synth_has_materials(int id)
{ return id == SND_STEP || id == SND_LAND || id == SND_DIG || id == SND_BREAK || id == SND_PLACE || id == SND_THUD; }

int synth_variants(int id)
{
    if (id == SND_STEP || id == SND_DIG) return 3; /* heard many times a minute */
    if (id == SND_UI_CLICK || id == SND_UI_HOVER || id == SND_UI_OPEN || id == SND_UI_CLOSE) return 1;
    return 2; /* the mixer's pitch jitter does the rest */
}

/* How an event plays a material: length, weight (lower, longer, more
 * body), brightness, and how much it crumbles apart. */
typedef struct {
    float dur, heavy, bright, crumble;
} hit_style;

static hit_style style_of(int id)
{
    switch (id) {
    case SND_STEP: return (hit_style){0.13f, 0.3f, 0.6f, 0.0f};
    case SND_DIG: return (hit_style){0.14f, 0.2f, 1.0f, 0.3f};
    case SND_PLACE: return (hit_style){0.2f, 0.7f, 0.5f, 0.0f};
    case SND_BREAK: return (hit_style){0.42f, 0.5f, 0.8f, 1.0f};
    case SND_LAND: return (hit_style){0.3f, 1.0f, 0.4f, 0.2f};
    default: return (hit_style){0.65f, 1.6f, 0.5f, 0.4f}; /* SND_THUD */
    }
}

/* One material struck, stepped on or broken. */
static int material_hit(float *buf, synth_material m, hit_style s, rng_t *r)
{
    int n = secs(s.dur * (m == MAT_GLASS ? 2.2f : 1.0f) + 0.05f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    float low = 1.0f / (1.0f + 0.35f * s.heavy); /* heavier: lower and longer */
    float d = s.dur * 0.3f / low;
    float body = 0.25f + 0.5f * s.heavy;
    int crumble = (int)(s.crumble * 30.0f);
    switch (m) {
    case MAT_GRASS:
        add_noise(buf, n, 0, n, 0.8f, 1800.0f, 7000.0f, 0.004f, d * 0.8f, r);
        add_grains(buf, n, 0, n / 2, 6 + crumble, 0.004f, 3000.0f, 1.5f, 0.5f, r);
        add_thump(buf, n, 0, 110.0f * low, body * 0.5f, d * 0.5f, r);
        break;
    case MAT_LEAVES:
        add_noise(buf, n, 0, n, 0.7f, 2500.0f, 9000.0f, 0.006f, d * 1.2f, r);
        add_grains(buf, n, 0, n * 3 / 4, 18 + crumble, 0.003f, 4200.0f, 2.0f, 0.4f, r);
        break;
    case MAT_SAND:
        add_grains(buf, n, 0, n * 2 / 3, 55 + crumble * 3, 0.0015f, 4200.0f, 1.2f, 0.5f, r);
        add_noise(buf, n, 0, n, 0.35f, 0.0f, 700.0f, 0.004f, d * 0.6f, r);
        break;
    case MAT_GRAVEL:
        add_grains(buf, n, 0, n * 2 / 3, 24 + crumble * 2, 0.004f, 2200.0f, 1.5f, 0.8f, r);
        add_thump(buf, n, 0, 120.0f * low, body * 0.5f, d * 0.4f, r);
        break;
    case MAT_SNOW:
        add_grains(buf, n, 0, n * 2 / 3, 34 + crumble * 2, 0.003f, 1400.0f, 2.0f, 0.6f, r);
        for (int k = 0; k < 2; k++)
            add_chirp(buf, n, secs(0.01f + 0.04f * rnd(r)), secs(0.025f), 800.0f, 1300.0f + 300.0f * rnd(r), 0.08f,
                      0.01f);
        add_noise(buf, n, 0, n, 0.25f, 0.0f, 900.0f, 0.004f, d * 0.6f, r);
        break;
    case MAT_STONE: {
        static const mode M[] = {{1900, 0.010f, 1.0f},
                                 {3100, 0.007f, 0.6f},
                                 {4700, 0.005f, 0.4f},
                                 {6200, 0.004f, 0.3f}};
        add_click(buf, n, 0, 0.9f, s.bright, r);
        add_modes(buf, n, 0, 0.45f, M, 4, low, r);
        add_thump(buf, n, 0, 140.0f * low, body * 0.6f, d * 0.35f, r);
        add_grains(buf, n, secs(0.01f), n * 3 / 4, crumble * 2, 0.003f, 3500.0f, 1.8f, 0.6f, r);
        break;
    }
    case MAT_WOOD: {
        static const mode M[] = {
            {190, 0.045f, 1.0f}, {430, 0.030f, 0.7f}, {760, 0.020f, 0.5f}, {1350, 0.012f, 0.35f}, {2100, 0.008f, 0.2f}};
        add_click(buf, n, 0, 0.5f, s.bright * 0.6f, r);
        add_modes(buf, n, 0, 0.8f, M, 5, low * (0.9f + 0.2f * rnd(r)), r);
        add_grains(buf, n, secs(0.02f), n * 3 / 4, crumble, 0.006f, 1600.0f, 1.2f, 0.5f, r); /* splinters */
        break;
    }
    case MAT_GLASS: {
        static const mode M[] = {{2600, 0.09f, 1.0f},
                                 {4100, 0.07f, 0.7f},
                                 {5900, 0.05f, 0.5f},
                                 {8200, 0.035f, 0.35f},
                                 {10400, 0.025f, 0.2f}};
        add_click(buf, n, 0, 0.5f, 1.0f, r);
        add_modes(buf, n, 0, 0.5f, M, 5, 0.85f + 0.3f * rnd(r), r);
        break;
    }
    case MAT_WATER:
        add_band(buf, n, 0, n, 0.9f, 700.0f, 0.8f, 0.01f, d, r);
        for (int k = 0; k < 4; k++)
            add_chirp(buf, n, secs(0.02f + s.dur * 0.6f * rnd(r)), secs(0.03f), 350.0f + 200.0f * rnd(r),
                      800.0f + 400.0f * rnd(r), 0.3f, 0.012f);
        break;
    default: /* dirt, and anything else */
        add_band(buf, n, 0, n, 0.9f, 380.0f * low, 0.7f, 0.003f, d * 0.7f, r);
        add_band(buf, n, 0, n, 0.4f, 950.0f, 1.0f, 0.002f, d * 0.4f, r);
        add_grains(buf, n, 0, n / 2, 5 + crumble, 0.005f, 1300.0f, 1.2f, 0.5f, r);
        add_thump(buf, n, 0, 90.0f * low, body * 0.6f, d * 0.5f, r);
        break;
    }
    if (s.heavy > 1.2f) add_thump(buf, n, 0, 55.0f, 0.9f, 0.12f, r); /* a big block: the ground booms */
    return n;
}

/* ------------------------------------------------------------ one-shots */

static int shatter(float *buf, rng_t *r)
{
    int n = secs(0.9f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    add_noise(buf, n, 0, secs(0.2f), 0.8f, 2000.0f, 12000.0f, 0.001f, 0.05f, r);
    for (int k = 0; k < 70; k++) {
        float u = rnd(r);
        mode m[2] = {{3000.0f + 6000.0f * rnd(r), 0.02f + 0.1f * rnd(r), 1.0f},
                     {5000.0f + 5000.0f * rnd(r), 0.01f + 0.05f * rnd(r), 0.5f}};
        add_modes(buf, n, (int)((float)secs(0.6f) * u * u), 0.12f + 0.2f * rnd(r), m, 2, 1.0f, r);
    }
    return n;
}

static int splash(float *buf, rng_t *r)
{
    int n = secs(0.9f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    add_noise(buf, n, 0, secs(0.35f), 1.0f, 300.0f, 5000.0f, 0.005f, 0.08f, r);
    add_band(buf, n, 0, secs(0.5f), 0.6f, 500.0f, 0.7f, 0.01f, 0.15f, r);
    for (int k = 0; k < 18; k++) {
        float u = rnd(r);
        add_chirp(buf, n, secs(0.05f + 0.7f * u * u), secs(0.02f + 0.03f * rnd(r)), 300.0f + 300.0f * rnd(r),
                  700.0f + 900.0f * rnd(r), 0.25f * (1.0f - u * 0.7f), 0.012f);
    }
    return n;
}

static int swim(float *buf, rng_t *r)
{
    int n = secs(0.5f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    add_band(buf, n, 0, n, 1.0f, 650.0f, 0.6f, 0.08f, 0.12f, r);
    for (int k = 0; k < 3; k++)
        add_chirp(buf, n, secs(0.1f + 0.25f * rnd(r)), secs(0.03f), 400.0f, 900.0f + 300.0f * rnd(r), 0.2f, 0.012f);
    return n;
}

/* The hurt grunt: a glottal pulse train with falling pitch through two
 * formants (an "uh"), over a body thump. */
static int hurt(float *buf, rng_t *r)
{
    int n = secs(0.32f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    biquad f1 = bandpass(620.0f * (0.9f + 0.2f * rnd(r)), 5.0f), f2 = bandpass(1100.0f, 6.0f);
    float ph = 0.0f, f0 = 125.0f + 30.0f * rnd(r);
    env e = env_start(0.01f, 0.09f);
    for (int i = 0; i < n; i++) {
        float t = (float)i / FS;
        ph += (f0 * (1.0f - 0.35f * t / 0.32f)) / FS;
        float pulse = ph - floorf(ph) < 0.08f ? 1.0f : 0.0f; /* a narrow pulse per period */
        float x = pulse + 0.15f * rnd_s(r);                  /* breathiness */
        buf[i] = (bq_run(&f1, x) + 0.6f * bq_run(&f2, x)) * env_next(&e) * 2.0f;
    }
    add_thump(buf, n, 0, 90.0f, 0.4f, 0.03f, r);
    return n;
}

static int eat(float *buf, rng_t *r)
{
    int n = secs(1.0f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    for (int c = 0; c < 3; c++) {
        int at = secs(0.05f + 0.3f * (float)c);
        add_grains(buf, n, at, secs(0.12f), 16, 0.004f, 2600.0f, 1.3f, 0.7f - 0.15f * (float)c, r);
        add_thump(buf, n, at, 180.0f, 0.2f, 0.02f, r);
    }
    return n;
}

static int drink(float *buf, rng_t *r)
{
    int n = secs(1.0f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    for (int g = 0; g < 3; g++) {
        int at = secs(0.08f + 0.28f * (float)g);
        add_chirp(buf, n, at, secs(0.09f), 190.0f, 120.0f, 0.8f, 0.04f);
        add_band(buf, n, at, secs(0.08f), 0.3f, 500.0f, 1.5f, 0.005f, 0.02f, r);
    }
    return n;
}

static int pour(float *buf, rng_t *r, int fill)
{
    int n = secs(0.7f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    /* Filling: the resonance rises as the air column shortens. */
    biquad b = bandpass(fill ? 500.0f : 700.0f, 3.0f);
    env e = env_start(0.03f, 0.35f);
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        if (fill && i % 256 == 0) {
            biquad nb = bandpass(500.0f + 900.0f * t, 3.0f);
            nb.z1 = b.z1;
            nb.z2 = b.z2;
            b = nb;
        }
        buf[i] = bq_run(&b, rnd_s(r)) * env_next(&e) * 1.5f;
    }
    for (int k = 0; k < 8; k++)
        add_chirp(buf, n, secs(0.05f + 0.5f * rnd(r)), secs(0.025f), 400.0f, 900.0f + 500.0f * rnd(r), 0.2f, 0.01f);
    return n;
}

static int fire_feed(float *buf, rng_t *r)
{
    int n = material_hit(buf, MAT_WOOD, style_of(SND_PLACE), r);
    for (int k = 0; k < 10; k++) add_click(buf, n, secs(0.03f + 0.15f * rnd(r)), 0.35f * rnd(r), 0.6f, r);
    return n;
}

static int fire_out(float *buf, rng_t *r)
{
    int n = secs(1.0f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    add_noise(buf, n, 0, n, 1.0f, 2500.0f, 9000.0f, 0.02f, 0.3f, r); /* steam hiss */
    return n;
}

/* Lub-dub: the mitral/tricuspid then the aortic/pulmonary valves closing.
 * The fundamentals (50-70 Hz) sit below small speakers, so a harmonic is
 * layered on top to be heard everywhere. */
static int heartbeat(float *buf, rng_t *r)
{
    int n = secs(0.5f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    add_thump(buf, n, 0, 55.0f, 1.0f, 0.045f, r);
    add_thump(buf, n, 0, 115.0f, 0.35f, 0.03f, r);
    int at = secs(0.19f);
    add_thump(buf, n, at, 70.0f, 0.7f, 0.035f, r);
    add_thump(buf, n, at, 140.0f, 0.25f, 0.02f, r);
    return n;
}

/* A laboured breath: in, then a longer, rougher out. */
static int breath(float *buf, rng_t *r)
{
    int n = secs(1.1f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    biquad b = bandpass(1100.0f, 0.9f), c = bandpass(2400.0f, 1.2f);
    for (int i = 0; i < n; i++) {
        float t = (float)i / FS;
        float inhale = t < 0.4f ? sinf(3.14159f * t / 0.4f) * 0.6f : 0.0f;
        float exhale = t > 0.45f ? sinf(3.14159f * (t - 0.45f) / 0.62f) : 0.0f;
        float x = rnd_s(r);
        buf[i] = (bq_run(&b, x) * (inhale + exhale) + 0.4f * bq_run(&c, x) * exhale) * 1.3f;
    }
    return n;
}

/* Interface sounds: short, soft, tuned. */
static int blip(float *buf, float f0, float f1, float len_s, float amp)
{
    int n = secs(len_s);
    memset(buf, 0, sizeof(float) * (size_t)n);
    add_chirp(buf, n, 0, n, f0, f1, amp, len_s * 0.35f);
    return n;
}

static int ui_click(float *buf, rng_t *r)
{
    int n = secs(0.05f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    add_chirp(buf, n, 0, secs(0.02f), 2100.0f, 1900.0f, 0.8f, 0.004f);
    add_click(buf, n, 0, 0.3f, 0.5f, r);
    return n;
}

static int craft(float *buf, rng_t *r)
{
    int n = material_hit(buf, MAT_WOOD, style_of(SND_PLACE), r);
    int m = secs(0.45f);
    if (m > n) {
        memset(buf + n, 0, sizeof(float) * (size_t)(m - n));
        n = m;
    }
    add_chirp(buf, n, secs(0.08f), secs(0.3f), 880.0f, 880.0f, 0.35f, 0.1f);
    add_chirp(buf, n, secs(0.14f), secs(0.3f), 1320.0f, 1320.0f, 0.3f, 0.1f);
    return n;
}

static int pickup(float *buf, rng_t *r)
{
    int n = blip(buf, 600.0f, 1500.0f, 0.07f, 0.8f);
    add_click(buf, n, 0, 0.2f, 0.4f, r);
    return n;
}

static int drop(float *buf, rng_t *r)
{
    int n = secs(0.16f);
    memset(buf, 0, sizeof(float) * (size_t)n);
    add_band(buf, n, 0, n, 1.0f, 900.0f, 0.8f, 0.03f, 0.04f, r); /* a short whoosh */
    return n;
}

/* ------------------------------------------------------------ loops */

#define LOOP_S 3.0f
#define LOOP_XF_S 0.25f

/* Wind: low noise with slow gusts and a faint whistle that wanders. */
static int wind(float *buf, rng_t *r)
{
    int n = secs(LOOP_S + LOOP_XF_S);
    float cl = pole(420.0f), l1 = 0.0f, l2 = 0.0f;
    biquad w = bandpass(650.0f, 7.0f);
    float g1 = rnd(r) * TAU, g2 = rnd(r) * TAU;
    for (int i = 0; i < n; i++) {
        float t = (float)i / FS, x = rnd_s(r);
        l1 += cl * (x - l1);
        l2 += cl * (l1 - l2);
        if (i % 512 == 0) {
            biquad nw = bandpass(550.0f + 250.0f * sinf(TAU * 0.13f * t + g2), 7.0f);
            nw.z1 = w.z1;
            nw.z2 = w.z2;
            w = nw;
        }
        float gust = 0.6f + 0.4f * sinf(TAU * 0.25f * t + g1) * sinf(TAU * 0.11f * t + g2);
        buf[i] = gust * (3.0f * l2 + 0.25f * bq_run(&w, x));
    }
    return n;
}

/* A brook: soft rushing noise and scattered bubbles. */
static int water(float *buf, rng_t *r)
{
    int n = secs(LOOP_S + LOOP_XF_S);
    memset(buf, 0, sizeof(float) * (size_t)n);
    add_noise(buf, n, 0, n, 0.35f, 200.0f, 1400.0f, 0.001f, 1e6f, r);
    for (int k = 0; k < 90; k++)
        add_chirp(buf, n, (int)(rnd(r) * (float)(n - secs(0.05f))), secs(0.015f + 0.03f * rnd(r)),
                  250.0f + 400.0f * rnd(r), 600.0f + 1400.0f * rnd(r), 0.08f + 0.12f * rnd(r), 0.01f);
    return n;
}

/* A campfire: a low roar that breathes, crackles of all sizes (a few
 * loud, many small), and the odd pop of steam in the wood. */
static int fire(float *buf, rng_t *r)
{
    int n = secs(LOOP_S + LOOP_XF_S);
    memset(buf, 0, sizeof(float) * (size_t)n);
    float cl = pole(250.0f), l1 = 0.0f, l2 = 0.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / FS, x = rnd_s(r);
        l1 += cl * (x - l1);
        l2 += cl * (l1 - l2);
        buf[i] = 2.5f * l2 * (0.7f + 0.3f * sinf(TAU * 0.7f * t));
    }
    for (int k = 0; k < 220; k++) {
        float size = rnd(r);
        size = size * size * size; /* mostly small */
        add_click(buf, n, (int)(rnd(r) * (float)(n - secs(0.01f))), 0.1f + 0.9f * size, 0.3f + 0.5f * rnd(r), r);
    }
    for (int k = 0; k < 4; k++)
        add_thump(buf, n, (int)(rnd(r) * (float)(n - secs(0.2f))), 200.0f + 100.0f * rnd(r), 0.3f, 0.01f, r);
    return n;
}

/* ------------------------------------------------------------ the bank */

static float gain_of(int id)
{
    static const float G[SND_COUNT] = {
        [SND_STEP] = 0.35f,     [SND_LAND] = 0.7f,      [SND_DIG] = 0.5f,         [SND_BREAK] = 0.7f,
        [SND_PLACE] = 0.6f,     [SND_SHATTER] = 0.8f,   [SND_THUD] = 0.9f,        [SND_SPLASH] = 0.7f,
        [SND_SWIM] = 0.4f,      [SND_PICKUP] = 0.35f,   [SND_DROP] = 0.35f,       [SND_HURT] = 0.7f,
        [SND_EAT] = 0.5f,       [SND_DRINK] = 0.5f,     [SND_BUCKET_FILL] = 0.5f, [SND_BUCKET_POUR] = 0.5f,
        [SND_FIRE_FEED] = 0.5f, [SND_FIRE_OUT] = 0.5f,  [SND_HEARTBEAT] = 0.9f,   [SND_BREATH] = 0.45f,
        [SND_UI_CLICK] = 0.3f,  [SND_UI_HOVER] = 0.12f, [SND_UI_OPEN] = 0.25f,    [SND_UI_CLOSE] = 0.25f,
        [SND_CRAFT] = 0.4f,
    };
    return id >= 0 && id < SND_COUNT ? G[id] : 0.5f;
}

/* Renders one clip into buf, returning its length. */
static int render_one(float *buf, int id, synth_material m, rng_t *r)
{
    if (synth_has_materials(id)) return material_hit(buf, m, style_of(id), r);
    switch (id) {
    case SND_SHATTER: return shatter(buf, r);
    case SND_SPLASH: return splash(buf, r);
    case SND_SWIM: return swim(buf, r);
    case SND_PICKUP: return pickup(buf, r);
    case SND_DROP: return drop(buf, r);
    case SND_HURT: return hurt(buf, r);
    case SND_EAT: return eat(buf, r);
    case SND_DRINK: return drink(buf, r);
    case SND_BUCKET_FILL: return pour(buf, r, 1);
    case SND_BUCKET_POUR: return pour(buf, r, 0);
    case SND_FIRE_FEED: return fire_feed(buf, r);
    case SND_FIRE_OUT: return fire_out(buf, r);
    case SND_HEARTBEAT: return heartbeat(buf, r);
    case SND_BREATH: return breath(buf, r);
    case SND_UI_CLICK: return ui_click(buf, r);
    case SND_UI_HOVER: return blip(buf, 3000.0f, 3000.0f, 0.02f, 0.5f);
    case SND_UI_OPEN: return blip(buf, 660.0f, 990.0f, 0.09f, 0.7f);
    case SND_UI_CLOSE: return blip(buf, 990.0f, 660.0f, 0.09f, 0.7f);
    case SND_CRAFT: return craft(buf, r);
    default: return 0;
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

int synth_build(synth_bank *b, uint32_t seed)
{
    double t0 = now_ms();
    memset(b, 0, sizeof *b);
    /* Material sounds: ~4.7 s per material over the six ids and their
     * variants, 11 materials; the rest ~20 s; three 3 s loops: ~85 s, 4 MB
     * of 16-bit samples. The cap leaves a margin. */
    b->cap = (uint32_t)(SYNTH_RATE * 100);
    b->pcm = mem_alloc(sizeof(int16_t) * b->cap);
    float *buf = mem_alloc(sizeof(float) * (size_t)MAX_CLIP);
    rng_t r = {seed ? seed : 0x5EEDu};
    for (int id = 0; id < SND_COUNT; id++) {
        int mats = synth_has_materials(id) ? MAT_COUNT : 1;
        for (int m = 0; m < mats; m++)
            for (int v = 0; v < synth_variants(id); v++) {
                int n = render_one(buf, id, (synth_material)m, &r);
                b->clip[id][m][v] = finish(b, buf, n, gain_of(id));
            }
    }
    static const float LOOP_GAIN[LOOP_COUNT] = {0.35f, 0.4f, 0.5f};
    int xf = secs(LOOP_XF_S), n = secs(LOOP_S);
    wind(buf, &r);
    b->loop[LOOP_WIND] = finish_loop(b, buf, n, xf, LOOP_GAIN[LOOP_WIND]);
    water(buf, &r);
    b->loop[LOOP_WATER] = finish_loop(b, buf, n, xf, LOOP_GAIN[LOOP_WATER]);
    fire(buf, &r);
    b->loop[LOOP_FIRE] = finish_loop(b, buf, n, xf, LOOP_GAIN[LOOP_FIRE]);
    mem_free(buf);
    b->build_ms = now_ms() - t0;
    return b->used <= b->cap ? 0 : -1;
}

void synth_free(synth_bank *b)
{
    mem_free(b->pcm);
    memset(b, 0, sizeof *b);
}

const synth_clip *synth_get(const synth_bank *b, int id, synth_material m, int variant)
{
    static const synth_clip SILENT = {NULL, 0, 0.0f};
    if (id < 0 || id >= SND_COUNT) return &SILENT;
    if (!synth_has_materials(id) || m < 0 || m >= MAT_COUNT) m = MAT_GENERIC;
    int nv = synth_variants(id);
    variant = ((variant % nv) + nv) % nv;
    const synth_clip *c = &b->clip[id][m][variant];
    if (!c->len) c = &b->clip[id][MAT_GENERIC][variant];
    return c->len ? c : &SILENT;
}
