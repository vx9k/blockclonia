/* The mixer and the synthesized sound bank (sound.c, sound_synth.c),
 * rendered offline: no audio device. */
#include "block.h"
#include "mem.h"
#include "sound.h"
#include "sound_synth.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAMES 48000 /* one second at SOUND_RATE */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

/* Renders `frames` in device-sized pieces; returns the peak and fills the
 * energy of each channel. */
static float render(sound *s, float *buf, int frames, double *el, double *er)
{
    float peak = 0.0f;
    *el = *er = 0.0;
    for (int done = 0; done < frames; done += 480) {
        int n = frames - done < 480 ? frames - done : 480;
        sound_render(s, buf + 2 * done, (uint32_t)n);
    }
    for (int i = 0; i < frames; i++) {
        float l = buf[2 * i], r = buf[2 * i + 1];
        peak = fmaxf(peak, fmaxf(fabsf(l), fabsf(r)));
        *el += (double)(l * l);
        *er += (double)(r * r);
    }
    return peak;
}

static int all_finite(const float *buf, int n)
{
    for (int i = 0; i < n; i++)
        if (!isfinite(buf[i])) return 0;
    return 1;
}

/* Energy above ~2 kHz: the difference between neighbours is a crude high pass. */
static double hf_energy(const float *buf, int frames)
{
    double e = 0.0;
    for (int i = 1; i < frames; i++) {
        double d = (double)(buf[2 * i] - buf[2 * i - 2]);
        e += d * d;
    }
    return e;
}

static sound *fresh(uint32_t seed)
{
    sound *s = sound_create(seed, SOUND_RATE, SOUND_CHANNELS);
    sound_listener(s, dv3(0, 0, 0), 0.0f, 0.0f, 0);
    sound_ambience(s, 0.0f, 0.0f);
    sound_update(s, 0.016f);
    return s;
}

static void test_bank(void)
{
    synth_bank *bp = mem_alloc(sizeof *bp), *cp = mem_alloc(sizeof *cp); /* ~17 KB each */
    CHECK(synth_build(bp, 7) == 0);
    printf("  sound bank: %.1f ms to synthesise, %.2f MB of samples\n", bp->build_ms,
           (double)bp->used * 2.0 / 1048576.0);
    CHECK(bp->used * 2u < 5u * 1048576u);
    /* Every sound and material has a playable, non-silent clip. */
    for (int id = 0; id < SND_COUNT; id++)
        for (int m = 0; m < MAT_COUNT; m++)
            for (int v = 0; v < SYNTH_VARIANTS; v++) {
                const synth_clip *c = synth_get(bp, id, (synth_material)m, v);
                int loud = 0;
                for (uint32_t i = 0; i < c->len; i++) loud |= abs(c->pcm[i]) > 3000;
                CHECK(c->len > 100 && loud && c->gain > 0.0f);
            }
    /* Clips start and end at silence (no clicks). */
    const synth_clip *step = synth_get(bp, SND_STEP, MAT_STONE, 0);
    CHECK(abs(step->pcm[0]) < 200 && abs(step->pcm[step->len - 1]) < 200);
    /* Loops are seamless: the jump from the last sample to the first is an
     * ordinary step for that sound, not an outlier (at least 0.5% of the
     * steps inside the loop are as big; a click would be bigger than all). */
    for (int l = 0; l < LOOP_COUNT; l++) {
        const synth_clip *c = &bp->loop[l];
        CHECK(c->len > 0);
        int wrap = abs(c->pcm[0] - c->pcm[c->len - 1]);
        uint32_t as_big = 0;
        for (uint32_t i = 1; i < c->len; i++) as_big += abs(c->pcm[i] - c->pcm[i - 1]) >= wrap;
        CHECK(as_big * 200u >= c->len);
    }
    /* Materials map as expected. */
    CHECK(synth_material_of(B_PLANKS) == MAT_WOOD && synth_material_of(B_ICE) == MAT_GLASS &&
          synth_material_of(B_AIR) == MAT_GENERIC);
    /* Deterministic for a seed. */
    synth_build(cp, 7);
    CHECK(cp->used == bp->used && memcmp(bp->pcm, cp->pcm, (size_t)bp->used * 2u) == 0);
    synth_free(cp);
    synth_free(bp);
    mem_free(cp);
    mem_free(bp);
}

static void test_mixing(void)
{
    float *buf = mem_alloc(sizeof(float) * FRAMES * 2);
    double el, er;

    /* Nothing playing: silence. */
    sound *s = fresh(1);
    CHECK(render(s, buf, FRAMES, &el, &er) == 0.0f);

    /* A footstep at the listener: audible, bounded. */
    sound_play(s, SND_STEP, NULL, B_STONE, 1.0f);
    float p = render(s, buf, FRAMES / 4, &el, &er);
    CHECK(p > 0.01f && p <= 1.0f && all_finite(buf, FRAMES / 2));

    /* Distance: 16 m is clearly quieter than 2 m (inverse distance). */
    sound *near = fresh(2), *far = fresh(2);
    dvec3 at2 = dv3(0, 0, -2), at16 = dv3(0, 0, -16);
    sound_play(near, SND_BREAK, &at2, B_STONE, 1.0f);
    sound_play(far, SND_BREAK, &at16, B_STONE, 1.0f);
    double en_l, en_r, ef_l, ef_r;
    render(near, buf, FRAMES / 2, &en_l, &en_r);
    render(far, buf, FRAMES / 2, &ef_l, &ef_r);
    CHECK(ef_l + ef_r < 0.2 * (en_l + en_r) && ef_l + ef_r > 0.0);

    /* Panning: facing north (-Z), a sound to the east (+X) is louder right. */
    sound *pan = fresh(3);
    dvec3 east = dv3(4, 0, 0);
    sound_play(pan, SND_DIG, &east, B_PLANKS, 1.0f);
    render(pan, buf, FRAMES / 4, &el, &er);
    CHECK(er > 4.0 * el);

    /* Under water: the same splash has much less high-frequency energy. */
    sound *dry = fresh(4), *wet = fresh(4);
    sound_listener(wet, dv3(0, 0, 0), 0.0f, 0.0f, 1);
    sound_update(wet, 0.016f);
    render(wet, buf, 9600, &el, &er); /* let the muffling fade in */
    render(dry, buf, 9600, &el, &er);
    sound_play(dry, SND_SHATTER, NULL, 0, 1.0f);
    sound_play(wet, SND_SHATTER, NULL, 0, 1.0f);
    render(dry, buf, FRAMES / 2, &el, &er);
    double hf_dry = hf_energy(buf, FRAMES / 2);
    render(wet, buf, FRAMES / 2, &el, &er);
    double hf_wet = hf_energy(buf, FRAMES / 2);
    CHECK(hf_wet < 0.3 * hf_dry);

    /* 500 sounds at once: at most 48 voices, no NaN, never beyond +-1. */
    sound *crowd = fresh(5);
    for (int i = 0; i < 500; i++) {
        int row = i / 20; /* a 20 x 25 grid of sources */
        dvec3 at = dv3((double)(i % 20) - 10.0, 0.0, (double)row - 12.0);
        sound_play(crowd, i % 2 ? SND_THUD : SND_SHATTER, &at, B_STONE, 2.0f);
    }
    p = render(crowd, buf, FRAMES / 2, &el, &er);
    int voices;
    float load;
    sound_stats(crowd, &voices, &load);
    CHECK(voices <= 48 && voices > 10 && p <= 1.0f && all_finite(buf, FRAMES));

    /* Ambience: wind and a fire loop keep playing; levels follow. */
    sound *amb = fresh(6);
    dvec3 fire = dv3(3, 0, 0);
    sound_ambience(amb, 1.0f, 0.5f);
    sound_fires(amb, &fire, 1);
    sound_update(amb, 0.016f);
    render(amb, buf, FRAMES, &el, &er);
    CHECK(el > 1.0 && er > el && all_finite(buf, FRAMES * 2)); /* the fire is to the right */

    /* The command ring: flooding it drops commands, never overruns. */
    sound *flood = fresh(7);
    for (int i = 0; i < 5000; i++) sound_play(flood, SND_UI_HOVER, NULL, 0, 0.1f);
    render(flood, buf, 480, &el, &er);
    sound_play(flood, SND_UI_CLICK, NULL, 0, 1.0f); /* works again once drained */
    CHECK(render(flood, buf, 4800, &el, &er) > 0.0f);

    /* Mixing cost: 48 busy voices, 10 ms of stereo. */
    sound *bench = fresh(8);
    for (int i = 0; i < 48; i++) {
        dvec3 at = dv3((double)i * 0.5, 0.0, -3.0);
        sound_play(bench, SND_THUD, &at, B_STONE, 1.0f);
    }
    double t0 = now_ms();
    for (int k = 0; k < 20; k++) sound_render(bench, buf, 480);
    double per = (now_ms() - t0) / 20.0;
    printf("  sound mixer: %.3f ms per 10 ms buffer with 48 voices\n", per);
    CHECK(per < 2.0);

    /* Silence stays exact zero after everything finishes (no DC, no denormals). */
    sound *tail = fresh(9);
    sound_play(tail, SND_UI_CLICK, NULL, 0, 1.0f);
    render(tail, buf, FRAMES, &el, &er);
    render(tail, buf, 4800, &el, &er);
    CHECK(el + er < 1e-9);

    sound *list[] = {s, near, far, pan, dry, wet, crowd, amb, flood, bench, tail};
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) sound_destroy(list[i]);
    mem_free(buf);
}

void test_sound_all(void)
{
    test_bank();
    test_mixing();
}
