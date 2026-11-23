#include "audition.h"
#include "log.h"
#include "mem.h"
#include "save.h"
#include "sound.h"
#include "sound_synth.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Names in file names and index.csv, in sound_id and synth_material order. */
static const char *const SOUND_NAMES[SND_COUNT] = {
    "step",      "land",   "dig",      "break",    "place",   "shatter",     "thud",        "splash",    "swim",
    "pickup",    "drop",   "hurt",     "eat",      "drink",   "bucket-fill", "bucket-pour", "fire-feed", "fire-out",
    "heartbeat", "breath", "ui-click", "ui-hover", "ui-open", "ui-close",    "craft",       "shot",      "hit",
};
static const char *const MATERIAL_NAMES[MAT_COUNT] = {"generic", "grass", "dirt",   "stone", "wood", "sand",
                                                      "gravel",  "snow",  "leaves", "glass", "water"};

/* Where the source is, with the listener at the origin facing -z. The
 * distances sit inside every sound's range today (18 m for the shortest),
 * so each setting has something to hear. */
typedef struct {
    const char *name;
    float dist, angle; /* m; degrees to the right of straight ahead */
    int at_head;       /* the body's own sounds: no position at all */
    int underwater;    /* the listener's head under water */
    int every_take;    /* every material and variant, not one take a sound */
} setting;

static const setting SETTINGS[] = {
    {"dry", 0.0f, 0.0f, 1, 0, 1},       /* as synthesized, centred */
    {"near", 2.0f, 0.0f, 0, 0, 0},      /* an arm's length ahead */
    {"right", 8.0f, 60.0f, 0, 0, 0},    /* off to one side, for panning */
    {"far", 16.0f, 0.0f, 0, 0, 0},      /* across a clearing */
    {"underwater", 3.0f, 0.0f, 0, 1, 0} /* close, heard with the head under */
};
#define SETTING_COUNT (int)(sizeof SETTINGS / sizeof SETTINGS[0])

/* The one take each sound plays in the settings that don't play them all:
 * stone, the material most of the world is made of. */
#define ONE_MATERIAL MAT_STONE

#define LOOP_SECONDS 4.0f
#define MAX_TAKE_SECONDS 8.0f
#define SETTLE_SECONDS 0.6f /* the limiter, ambience and underwater blend come to rest */
#define TAIL_SECONDS 0.25f
#define BLOCK 1024u

size_t audition_wav(const float *stereo, uint32_t frames, uint32_t rate, uint8_t **out)
{
    const uint32_t data = frames * 4u, size = 44u + data;
    uint8_t *w = mem_alloc(size);
    const uint32_t fields[] = {0x46464952u /* RIFF */,
                               size - 8u,
                               0x45564157u /* WAVE */,
                               0x20746d66u /* fmt */,
                               16u,
                               1u | (2u << 16) /* PCM, stereo */,
                               rate,
                               rate * 4u,
                               4u | (16u << 16) /* block align, bits */,
                               0x61746164u /* data */,
                               data};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
        for (int b = 0; b < 4; b++) w[4 * i + (size_t)b] = (uint8_t)(fields[i] >> (8 * b));
    for (uint32_t i = 0; i < frames * 2u; i++) {
        float x = stereo[i] > 1.0f ? 1.0f : (stereo[i] < -1.0f ? -1.0f : stereo[i]);
        int16_t v = (int16_t)lrintf(x * 32767.0f);
        w[44u + 2u * i] = (uint8_t)((uint16_t)v & 0xffu);
        w[45u + 2u * i] = (uint8_t)((uint16_t)v >> 8);
    }
    *out = w;
    return size;
}

/* In-place radix-2 FFT of n (a power of two) complex values. */
static void fft(float *re, float *im, uint32_t n)
{
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (uint32_t len = 2; len <= n; len <<= 1) {
        const double ang = -6.283185307179586 / (double)len;
        for (uint32_t i = 0; i < n; i += len)
            for (uint32_t k = 0; k < len / 2; k++) {
                const float wr = (float)cos(ang * (double)k), wi = (float)sin(ang * (double)k);
                const uint32_t a = i + k, b = a + len / 2;
                const float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
    }
}

static float to_db(double x) { return x > 1e-6 ? (float)(20.0 * log10(x)) : AUDITION_SILENT_DB; }

void audition_measure(const float *stereo, uint32_t frames, uint32_t rate, audition_stats *st)
{
    double peak = 0.0, sum = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        const double m = 0.5 * ((double)stereo[2 * i] + (double)stereo[2 * i + 1]);
        peak = fmax(peak, fmax(fabs((double)stereo[2 * i]), fabs((double)stereo[2 * i + 1])));
        sum += m * m;
    }
    st->seconds = (float)frames / (float)rate;
    st->peak_db = to_db(peak);
    st->rms_db = frames ? to_db(sqrt(sum / (double)frames)) : AUDITION_SILENT_DB;

    /* The centroid of the summed power spectrum of Hann-windowed blocks,
     * half overlapping; a clip shorter than a block is zero-padded. */
    float re[BLOCK] = {0}, im[BLOCK] = {0};
    double weighted = 0.0, total = 0.0;
    for (uint32_t at = 0; at < frames || at == 0; at += BLOCK / 2) {
        for (uint32_t i = 0; i < BLOCK; i++) {
            const uint32_t f = at + i;
            const double hann = 0.5 - 0.5 * cos(6.283185307179586 * (double)i / (double)BLOCK);
            re[i] = f < frames ? (float)(hann * 0.5 * ((double)stereo[2 * f] + (double)stereo[2 * f + 1])) : 0.0f;
            im[i] = 0.0f;
        }
        fft(re, im, BLOCK);
        for (uint32_t k = 1; k < BLOCK / 2; k++) {
            const double p = (double)re[k] * (double)re[k] + (double)im[k] * (double)im[k];
            weighted += p * (double)k * (double)rate / (double)BLOCK;
            total += p;
        }
        if (at + BLOCK >= frames) break;
    }
    st->centroid_hz = total > 1e-12 ? (float)(weighted / total) : 0.0f;
}

/* A growing stereo buffer of rendered frames. */
typedef struct {
    float *f;
    uint32_t frames, cap;
} take_buf;

static void render_into(sound *s, take_buf *t, uint32_t frames)
{
    if (t->frames + frames > t->cap) {
        t->cap = (t->frames + frames) * 2u;
        t->f = mem_realloc(t->f, mem_array_size(t->cap, 2u * sizeof(float)));
    }
    sound_render(s, t->f + (size_t)t->frames * 2u, frames);
    t->frames += frames;
}

/* Puts the listener in a setting with nothing playing, then lets the
 * mixer's state come to rest so one take never colours the next. */
static void settle(sound *s, const setting *st, take_buf *scratch)
{
    sound_listener(s, (dvec3){0.0, 0.0, 0.0}, 0.0f, 0.0f, st->underwater);
    sound_ambience(s, 0.0f, 0.0f);
    sound_fires(s, NULL, 0);
    sound_update(s, 0.0f);
    scratch->frames = 0;
    render_into(s, scratch, (uint32_t)(SETTLE_SECONDS * (float)SOUND_RATE));
}

static dvec3 source_of(const setting *st)
{
    const double a = (double)st->angle * 3.141592653589793 / 180.0;
    return (dvec3){(double)st->dist * sin(a), 0.0, -(double)st->dist * cos(a)};
}

/* Writes one take and its line in the index. Returns 0, or -1. */
static int write_take(const char *dir, const setting *st, const char *name, const take_buf *t, FILE *index)
{
    char path[1024] = {0};
    if (snprintf(path, sizeof path, "%s/%s/%s.wav", dir, st->name, name) >= (int)sizeof path) return -1;
    uint8_t *wav = NULL;
    size_t len = audition_wav(t->f, t->frames, SOUND_RATE, &wav);
    int r = save_write_file(path, wav, len);
    mem_free(wav);
    if (r != 0) return -1;
    audition_stats m = {0};
    audition_measure(t->f, t->frames, SOUND_RATE, &m);
    fprintf(index, "%s,%s,%.3f,%.1f,%.1f,%.0f\n", st->name, name, (double)m.seconds, (double)m.peak_db,
            (double)m.rms_db, (double)m.centroid_hz);
    return 0;
}

/* Plays one take and renders until it has finished, plus a short tail. */
static void render_take(sound *s, const setting *st, int id, int mat, int var, take_buf *t)
{
    const dvec3 at = source_of(st);
    sound_play_take(s, id, st->at_head ? NULL : &at, mat, var, 1.0f);
    t->frames = 0;
    const uint32_t most = (uint32_t)(MAX_TAKE_SECONDS * (float)SOUND_RATE);
    int voices = 1;
    float load = 0.0f;
    while (voices > 0 && t->frames < most) {
        render_into(s, t, BLOCK);
        sound_stats(s, &voices, &load);
    }
    render_into(s, t, (uint32_t)(TAIL_SECONDS * (float)SOUND_RATE));
}

/* The ambience loops: wind and water all around, a fire at the source. */
static void render_loop(sound *s, const setting *st, int which, take_buf *t)
{
    const dvec3 at = source_of(st);
    sound_ambience(s, which == LOOP_WIND ? 1.0f : 0.0f, which == LOOP_WATER ? 1.0f : 0.0f);
    sound_fires(s, &at, which == LOOP_FIRE ? 1 : 0);
    sound_update(s, 0.0f);
    t->frames = 0;
    render_into(s, t, (uint32_t)(LOOP_SECONDS * (float)SOUND_RATE));
}

static int ensure_dirs(const char *dir)
{
    if (save_ensure_dir(dir) != 0) return -1;
    for (int i = 0; i < SETTING_COUNT; i++) {
        char sub[1024] = {0};
        if (snprintf(sub, sizeof sub, "%s/%s", dir, SETTINGS[i].name) >= (int)sizeof sub) return -1;
        if (save_ensure_dir(sub) != 0) return -1;
    }
    return 0;
}

int audition_render(const char *dir, uint32_t seed)
{
    if (ensure_dirs(dir) != 0) return -1;
    char index_path[1024] = {0};
    if (snprintf(index_path, sizeof index_path, "%s/index.csv", dir) >= (int)sizeof index_path) return -1;
    /* A text summary, rewritten whole; the WAVs go through save_write_file. */
    FILE *index = fopen(index_path, "w");
    if (!index) return -1;
    fprintf(index, "setting,take,seconds,peak_db,rms_db,centroid_hz\n");

    sound *s = sound_create(seed, SOUND_RATE, 2);
    sound_set_volumes(s, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    take_buf t = {0}, scratch = {0};
    int written = 0, failed = 0;
    for (int si = 0; si < SETTING_COUNT && !failed; si++) {
        const setting *st = &SETTINGS[si];
        for (int id = 0; id < SND_COUNT && !failed; id++)
            for (int mat = 0; mat < MAT_COUNT && !failed; mat++)
                for (int var = 0; var < SYNTH_VARIANTS && !failed; var++) {
                    if (!sound_take_len(s, id, mat, var)) continue;
                    if (!st->every_take && (var > 0 || (synth_has_materials(id) ? mat != ONE_MATERIAL : 0))) continue;
                    char name[96] = {0};
                    if (synth_has_materials(id))
                        snprintf(name, sizeof name, "%s-%s-%d", SOUND_NAMES[id], MATERIAL_NAMES[mat], var + 1);
                    else snprintf(name, sizeof name, "%s-%d", SOUND_NAMES[id], var + 1);
                    settle(s, st, &scratch);
                    render_take(s, st, id, mat, var, &t);
                    if (write_take(dir, st, name, &t, index) != 0) failed = 1;
                    else written++;
                }
        static const char *const LOOP_NAMES[LOOP_COUNT] = {"loop-wind", "loop-water", "loop-fire"};
        for (int l = 0; l < LOOP_COUNT && !failed; l++) {
            settle(s, st, &scratch);
            render_loop(s, st, l, &t);
            if (write_take(dir, st, LOOP_NAMES[l], &t, index) != 0) failed = 1;
            else written++;
        }
    }
    sound_destroy(s);
    mem_free(t.f);
    mem_free(scratch.f);
    if (fclose(index) != 0) failed = 1;
    if (failed) {
        log_error("sound render: cannot write into %s", dir);
        return -1;
    }
    log_info("sound render: %d takes in %s (index.csv lists them)", written, dir);
    return written;
}
