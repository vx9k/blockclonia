#include "sound.h"
#include "mem.h"
#include "os.h"
#include "sound_synth.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

/* The mixer.
 *
 * Two threads share this object. The game thread calls sound_play,
 * sound_listener and friends; they turn into fixed-size commands pushed
 * into a single-producer single-consumer ring (release on the head,
 * acquire on the read). The audio thread drains the ring at the start of
 * every sound_render and owns everything else: voices, filters, limiter.
 * Nothing on the audio path locks, allocates or makes a syscall except
 * one monotonic clock read for the load figure.
 *
 * Per voice: a mono 16-bit clip read at a fractional position (32.32 fixed
 * point) with linear interpolation, which also varies the pitch of each
 * play; gains recomputed per chunk from the listener (inverse distance
 * beyond a per-sound reference, faded out towards a maximum range,
 * equal-power panning, a little quieter behind) and ramped across the
 * chunk so moving sounds never click. Voices go to an effects or an
 * ambient bus; under water both pass through a one-pole low pass. The
 * master bus has a peak limiter (instant attack per chunk, 300 ms release)
 * and a soft clip above 0.9, so the output never leaves [-1, 1]. */

#define VOICES 48
#define RING 1024           /* commands; a frame posts ~15 */
#define CHUNK 256           /* frames mixed at a time */
#define FIXED_ONE 4294967296.0

typedef enum { CMD_PLAY, CMD_LISTENER, CMD_AMBIENCE, CMD_FIRE, CMD_FIRE_COUNT, CMD_VOLUMES } cmd_type;

typedef struct {
    uint8_t type, id, material, positional;
    /* pad fills the header to 8 bytes before the floats. */
    /* cppcheck-suppress unusedStructMember */
    uint8_t variant, index, underwater, pad;
    float a, b, c;          /* intensity/pitch, yaw/pitch, wind/water, volumes */
    double x, y, z;         /* world position */
} cmd;

typedef struct {
    const synth_clip *clip; /* NULL: free */
    uint64_t pos, step;     /* 32.32 fixed point, in clip samples */
    float gain;             /* clip gain x intensity */
    float gl, gr;           /* gains applied at the end of the last chunk */
    float ref, range;       /* m: full level within ref, silent beyond range */
    int positional;
    double x, y, z;
} voice;

typedef struct {
    const synth_clip *clip;
    uint64_t pos, step;
    float level, target;    /* 0..1, eased */
    float gl, gr;
    double x, y, z;         /* fires only */
} loop_voice;

struct sound {
    synth_bank bank;
    uint32_t rate;
    int channels;
    double step_base;       /* clip samples per output frame at pitch 1 */

    /* The command ring. */
    cmd ring[RING];
    _Atomic uint32_t head, tail;
    _Atomic uint32_t dropped;

    /* Game thread only. */
    uint32_t rng;
    double lx, ly, lz;
    float yaw, pitch;
    int underwater;
    float wind, water;
    dvec3 fires[SOUND_MAX_FIRES];
    int fire_count;
    float vol[3];           /* master, effects, ambient */
    int vol_dirty;

    /* Audio thread only. */
    voice v[VOICES];
    loop_voice wind_v, water_v, fire_v[SOUND_MAX_FIRES];
    int fires_on;
    double alx, aly, alz;
    float ayaw;
    int aunder;
    float under;            /* 0..1 eased underwater blend */
    float avol[3];
    float lp[4];            /* one-pole state: fx L, fx R, ambient L, ambient R */
    float lp_coef;
    float limit;            /* limiter gain */
    float fx[CHUNK * 2], amb[CHUNK * 2];

    /* Published by the audio thread. */
    _Atomic int stat_voices;
    _Atomic uint32_t stat_load; /* float bits */
};

/* ---------------------------------------------------------- game side */

static uint32_t next_rand(sound *s)
{
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return s->rng;
}

static void post(sound *s, const cmd *c)
{
    uint32_t head = atomic_load_explicit(&s->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    if (head - tail >= RING) {
        atomic_fetch_add_explicit(&s->dropped, 1u, memory_order_relaxed);
        return; /* full: the newest command is dropped */
    }
    s->ring[head % RING] = *c;
    atomic_store_explicit(&s->head, head + 1u, memory_order_release);
}

sound *sound_create(uint32_t seed, uint32_t rate, int channels)
{
    sound *s = mem_calloc(1, sizeof *s);
    s->rate = rate ? rate : SOUND_RATE;
    s->channels = channels == 1 ? 1 : 2;
    s->step_base = (double)SYNTH_RATE / (double)s->rate;
    s->rng = seed ? seed : 0x5A17u;
    synth_build(&s->bank, seed);
    s->vol[0] = s->vol[1] = s->vol[2] = 1.0f;
    s->avol[0] = s->avol[1] = s->avol[2] = 1.0f;
    s->limit = 1.0f;
    s->lp_coef = 1.0f - expf(-6.2831853f * 700.0f / (float)s->rate); /* muffled below ~700 Hz */
    uint64_t step = (uint64_t)(s->step_base * FIXED_ONE);
    s->wind_v = (loop_voice){&s->bank.loop[LOOP_WIND], 0, step, 0.0f, 0.0f, 0.707f, 0.707f, 0.0, 0.0, 0.0};
    s->water_v = (loop_voice){&s->bank.loop[LOOP_WATER], 0, step, 0.0f, 0.0f, 0.707f, 0.707f, 0.0, 0.0, 0.0};
    for (int i = 0; i < SOUND_MAX_FIRES; i++) {
        /* Different starting points so several fires do not crackle in step. */
        uint64_t start = ((uint64_t)s->bank.loop[LOOP_FIRE].len * (uint64_t)i / SOUND_MAX_FIRES) << 32;
        s->fire_v[i] = (loop_voice){&s->bank.loop[LOOP_FIRE], start, step, 0.0f, 0.0f, 0.0f, 0.0f, 0.0, 0.0, 0.0};
    }
    return s;
}

void sound_destroy(sound *s)
{
    if (!s) return;
    synth_free(&s->bank);
    mem_free(s);
}

void sound_set_volumes(sound *s, float master, float effects, float ambient)
{
    s->vol[0] = master;
    s->vol[1] = effects;
    s->vol[2] = ambient;
    s->vol_dirty = 1;
}

void sound_listener(sound *s, dvec3 pos, float yaw, float pitch, int underwater)
{
    s->lx = pos.x;
    s->ly = pos.y;
    s->lz = pos.z;
    s->yaw = yaw;
    s->pitch = pitch;
    s->underwater = underwater;
}

void sound_play(sound *s, int id, const dvec3 *pos, uint8_t material, float intensity)
{
    if (id < 0 || id >= SND_COUNT || !(intensity > 0.0f)) return;
    uint32_t r = next_rand(s);
    cmd c;
    memset(&c, 0, sizeof c);
    c.type = CMD_PLAY;
    c.id = (uint8_t)id;
    c.material = (uint8_t)synth_material_of(material);
    c.variant = (uint8_t)(r % 3u);
    c.a = intensity > 2.0f ? 2.0f : intensity;
    /* +-6% pitch so repeats never sound identical; heavier hits lower. */
    c.b = (0.94f + 0.12f * (float)((r >> 8) & 1023u) / 1023.0f) * (1.0f - 0.08f * (c.a > 1.0f ? c.a - 1.0f : 0.0f));
    if (pos) {
        c.positional = 1;
        c.x = pos->x;
        c.y = pos->y;
        c.z = pos->z;
    }
    post(s, &c);
}

void sound_ambience(sound *s, float wind, float water)
{
    s->wind = wind;
    s->water = water;
}

void sound_fires(sound *s, const dvec3 *pos, int n)
{
    n = n < 0 ? 0 : (n > SOUND_MAX_FIRES ? SOUND_MAX_FIRES : n);
    for (int i = 0; i < n; i++) s->fires[i] = pos[i];
    s->fire_count = n;
}

void sound_update(sound *s, float dt)
{
    (void)dt;
    cmd c;
    memset(&c, 0, sizeof c);
    c.type = CMD_LISTENER;
    c.x = s->lx;
    c.y = s->ly;
    c.z = s->lz;
    c.a = s->yaw;
    c.b = s->pitch;
    c.underwater = (uint8_t)(s->underwater != 0);
    post(s, &c);
    c.type = CMD_AMBIENCE;
    c.a = s->wind;
    c.b = s->water;
    post(s, &c);
    for (int i = 0; i < s->fire_count; i++) {
        c.type = CMD_FIRE;
        c.index = (uint8_t)i;
        c.x = s->fires[i].x;
        c.y = s->fires[i].y;
        c.z = s->fires[i].z;
        post(s, &c);
    }
    c.type = CMD_FIRE_COUNT;
    c.index = (uint8_t)s->fire_count;
    post(s, &c);
    if (s->vol_dirty) {
        c.type = CMD_VOLUMES;
        c.a = s->vol[0];
        c.b = s->vol[1];
        c.c = s->vol[2];
        post(s, &c);
        s->vol_dirty = 0;
    }
}

void sound_stats(const sound *s, int *voices, float *load)
{
    *voices = atomic_load_explicit(&s->stat_voices, memory_order_relaxed);
    uint32_t bits = atomic_load_explicit(&s->stat_load, memory_order_relaxed);
    float f;
    memcpy(&f, &bits, sizeof f);
    *load = f;
}

/* ---------------------------------------------------------- audio side */

/* How far and loud each sound carries: full level within ref metres,
 * silent beyond range. */
static void reach_of(int id, float *ref, float *range)
{
    static const float REACH[SND_COUNT][2] = {
        [SND_STEP] = {1.5f, 18.0f},      [SND_SWIM] = {2.0f, 18.0f},        [SND_DIG] = {2.0f, 24.0f},
        [SND_PLACE] = {2.0f, 24.0f},     [SND_DROP] = {2.0f, 24.0f},        [SND_BREAK] = {3.0f, 32.0f},
        [SND_LAND] = {3.0f, 32.0f},      [SND_BUCKET_FILL] = {3.0f, 32.0f}, [SND_BUCKET_POUR] = {3.0f, 32.0f},
        [SND_FIRE_FEED] = {3.0f, 32.0f}, [SND_SHATTER] = {4.0f, 48.0f},     [SND_SPLASH] = {4.0f, 48.0f},
        [SND_THUD] = {6.0f, 72.0f},
    };
    const float *r = id >= 0 && id < SND_COUNT ? REACH[id] : NULL;
    *ref = r && r[1] > 0.0f ? r[0] : 3.0f;
    *range = r && r[1] > 0.0f ? r[1] : 32.0f;
}

/* Gains for a source at (x, y, z) heard by the current listener. */
static float spatial(const sound *s, double x, double y, double z, float ref, float range, float *gl, float *gr)
{
    double dx = x - s->alx, dy = y - s->aly, dz = z - s->alz;
    float dist = (float)sqrt(dx * dx + dy * dy + dz * dz);
    float att = dist <= ref ? 1.0f : ref / dist;
    if (dist > range) att = 0.0f;
    else if (dist > 0.7f * range) att *= (range - dist) / (0.3f * range);
    float pan = 0.0f, front = 1.0f;
    float hl = (float)sqrt(dx * dx + dz * dz);
    if (hl > 0.05f) {
        float cy = cosf(s->ayaw), sy = sinf(s->ayaw);
        float nx = (float)dx / hl, nz = (float)dz / hl;
        pan = (nx * cy + nz * sy) * (hl / (hl + 0.5f)); /* near the head, less extreme */
        front = nx * sy - nz * cy;
    }
    att *= 0.82f + 0.18f * (0.5f + 0.5f * front); /* a little duller behind */
    float angle = (pan + 1.0f) * 0.78539816f;
    *gl = cosf(angle) * att;
    *gr = sinf(angle) * att;
    return att;
}

/* Adds a mono clip, resampled, into a stereo buffer with gains ramping
 * from (gl0, gr0) to (gl1, gr1). Returns 0 when a one-shot ran out. */
static int mix_clip(const synth_clip *clip, uint64_t *posp, uint64_t step, int loop, float *out, int frames, float gl0,
                    float gr0, float gl1, float gr1)
{
    const int16_t *pcm = clip->pcm;
    const uint64_t len = clip->len;
    if (!pcm || len < 2) return 0;
    const float k = clip->gain / 32768.0f, inv = 1.0f / (float)frames;
    const float dgl = (gl1 - gl0) * inv, dgr = (gr1 - gr0) * inv;
    float gl = gl0, gr = gr0;
    uint64_t pos = *posp;
    int i = 0;
    while (i < frames) {
        /* Frames that can run without reaching the last sample: the hot
         * loop needs no bounds test. */
        uint64_t last = (len - 1u) << 32;
        int run = frames - i;
        if (pos < last) {
            /* pos + (can - 1) * step stays below last: idx + 1 < len. */
            uint64_t can = (last - pos - 1u) / step + 1u;
            if (can < (uint64_t)run) run = (int)can;
        } else {
            run = 0;
        }
        for (int j = 0; j < run; j++, i++) {
            uint32_t idx = (uint32_t)(pos >> 32);
            float frac = (float)(uint32_t)pos * (1.0f / 4294967296.0f);
            float a = (float)pcm[idx], b = (float)pcm[idx + 1];
            float x = (a + (b - a) * frac) * k;
            out[2 * i] += x * gl;
            out[2 * i + 1] += x * gr;
            gl += dgl;
            gr += dgr;
            pos += step;
        }
        if (i >= frames) break;
        if (!loop) {
            *posp = pos;
            return 0;
        }
        /* The last sample interpolates towards the first, then wraps. */
        uint32_t idx = (uint32_t)(pos >> 32);
        if (idx >= len) {
            pos -= len << 32;
            continue;
        }
        float frac = (float)(uint32_t)pos * (1.0f / 4294967296.0f);
        float x = ((float)pcm[idx] + ((float)pcm[0] - (float)pcm[idx]) * frac) * k;
        out[2 * i] += x * gl;
        out[2 * i + 1] += x * gr;
        gl += dgl;
        gr += dgr;
        pos += step;
        i++;
        if ((pos >> 32) >= len) pos -= len << 32;
    }
    *posp = pos;
    return 1;
}

/* How loud a voice is now, to pick one to steal. */
static float loudness(const voice *v)
{
    float left = 1.0f - (float)(v->pos >> 32) / (float)(v->clip->len ? v->clip->len : 1u);
    return v->gain * fmaxf(v->gl, v->gr) * (0.3f + 0.7f * left);
}

static void start_voice(sound *s, const cmd *c)
{
    float ref, range, gl = 0.707f, gr = 0.707f, audible = 1.0f;
    reach_of(c->id, &ref, &range);
    if (c->positional) audible = spatial(s, c->x, c->y, c->z, ref, range, &gl, &gr);
    const synth_clip *clip = synth_get(&s->bank, c->id, (synth_material)c->material, c->variant);
    float gain = c->a;
    if (!clip->len || audible * gain * clip->gain < 0.002f) return; /* too far to hear */
    voice *slot = NULL;
    float quietest = 1e9f;
    for (int i = 0; i < VOICES; i++) {
        voice *v = &s->v[i];
        if (!v->clip) {
            slot = v;
            break;
        }
        float l = loudness(v);
        if (l < quietest) {
            quietest = l;
            slot = v;
        }
    }
    /* All busy: take the quietest, but only for something louder. */
    if (slot->clip && quietest >= audible * gain * 0.707f) return;
    slot->clip = clip;
    slot->pos = 0;
    slot->step = (uint64_t)(s->step_base * (double)c->b * FIXED_ONE);
    slot->gain = gain;
    slot->ref = ref;
    slot->range = range;
    slot->positional = c->positional;
    slot->x = c->x;
    slot->y = c->y;
    slot->z = c->z;
    /* Starts at its gains: the clip's own fade-in avoids a click. */
    slot->gl = gl;
    slot->gr = gr;
}

static void drain(sound *s)
{
    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&s->head, memory_order_acquire);
    for (; tail != head; tail++) {
        const cmd *c = &s->ring[tail % RING];
        switch (c->type) {
        case CMD_PLAY: start_voice(s, c); break;
        case CMD_LISTENER:
            s->alx = c->x;
            s->aly = c->y;
            s->alz = c->z;
            s->ayaw = c->a;
            s->aunder = c->underwater;
            break;
        case CMD_AMBIENCE:
            s->wind_v.target = c->a;
            s->water_v.target = c->b;
            break;
        case CMD_FIRE:
            if (c->index < SOUND_MAX_FIRES) {
                s->fire_v[c->index].x = c->x;
                s->fire_v[c->index].y = c->y;
                s->fire_v[c->index].z = c->z;
            }
            break;
        case CMD_FIRE_COUNT: s->fires_on = c->index; break;
        case CMD_VOLUMES:
            s->avol[0] = c->a;
            s->avol[1] = c->b;
            s->avol[2] = c->c;
            break;
        default: break;
        }
    }
    atomic_store_explicit(&s->tail, tail, memory_order_release);
}

/* One loop voice: its level eases to the target over ~0.3 s. */
static void mix_loop(loop_voice *lv, float *out, int frames, float chunk_s, float gl, float gr)
{
    float from = lv->level;
    lv->level += (lv->target - lv->level) * (1.0f - expf(-chunk_s / 0.3f));
    if (lv->target <= 0.0f && lv->level < 1e-6f) lv->level = 0.0f; /* faded out: exactly, no subnormals */
    if (from < 1e-4f && lv->level < 1e-4f) {
        lv->gl = gl * lv->level;
        lv->gr = gr * lv->level;
        return; /* silent: skip the work */
    }
    float gl1 = gl * lv->level, gr1 = gr * lv->level;
    mix_clip(lv->clip, &lv->pos, lv->step, 1, out, frames, lv->gl, lv->gr, gl1, gr1);
    lv->gl = gl1;
    lv->gr = gr1;
}

static void mix_chunk(sound *s, float *out, int frames)
{
    memset(s->fx, 0, sizeof(float) * (size_t)frames * 2u);
    memset(s->amb, 0, sizeof(float) * (size_t)frames * 2u);
    const float chunk_s = (float)frames / (float)s->rate;
    int active = 0;
    for (int i = 0; i < VOICES; i++) {
        voice *v = &s->v[i];
        if (!v->clip) continue;
        float gl = 0.707f, gr = 0.707f;
        if (v->positional) spatial(s, v->x, v->y, v->z, v->ref, v->range, &gl, &gr);
        gl *= v->gain;
        gr *= v->gain;
        if (!mix_clip(v->clip, &v->pos, v->step, 0, s->fx, frames, v->gl, v->gr, gl, gr)) v->clip = NULL;
        else active++;
        v->gl = gl;
        v->gr = gr;
    }
    /* Ambience: wind and water all around, fires where they are. */
    mix_loop(&s->wind_v, s->amb, frames, chunk_s, 0.707f, 0.707f);
    mix_loop(&s->water_v, s->amb, frames, chunk_s, 0.707f, 0.707f);
    for (int i = 0; i < SOUND_MAX_FIRES; i++) {
        loop_voice *f = &s->fire_v[i];
        float gl = 0.0f, gr = 0.0f;
        f->target = i < s->fires_on ? spatial(s, f->x, f->y, f->z, 1.5f, 20.0f, &gl, &gr) : 0.0f;
        if (f->target > 0.0f) {
            gl /= f->target;
            gr /= f->target;
        } else {
            gl = f->gl;
            gr = f->gr;
        }
        mix_loop(f, s->amb, frames, chunk_s, gl, gr);
    }
    atomic_store_explicit(&s->stat_voices, active, memory_order_relaxed);

    /* Under water: both buses through a low pass, blended in over 0.2 s. */
    s->under += ((float)s->aunder - s->under) * (1.0f - expf(-chunk_s / 0.2f));
    const float a = s->lp_coef, wet = s->under, dry = 1.0f - wet;
    const float gfx = 1.4f * s->avol[0] * s->avol[1], gamb = 1.4f * s->avol[0] * s->avol[2];
    float peak = 0.0f;
    for (int i = 0; i < frames; i++) {
        float mix[2];
        for (int ch = 0; ch < 2; ch++) {
            float x = s->fx[2 * i + ch], y = s->amb[2 * i + ch];
            s->lp[ch] += a * (x - s->lp[ch]);
            s->lp[2 + ch] += a * (y - s->lp[2 + ch]);
            mix[ch] = gfx * (dry * x + wet * s->lp[ch]) + gamb * (dry * y + wet * s->lp[2 + ch]);
        }
        s->fx[2 * i] = mix[0];
        s->fx[2 * i + 1] = mix[1];
        peak = fmaxf(peak, fmaxf(fabsf(mix[0]), fabsf(mix[1])));
    }
    for (int k = 0; k < 4; k++)
        if (fabsf(s->lp[k]) < 1e-15f) s->lp[k] = 0.0f; /* no denormals in a silent tail */

    /* Limiter: drop at once to keep the peak under 0.95, recover slowly;
     * the gain ramps across the chunk, and a soft clip catches the rest. */
    float target = peak * s->limit > 0.95f ? 0.95f / peak
                                           : s->limit + (1.0f - s->limit) * (1.0f - expf(-chunk_s / 0.3f));
    float dg = (target - s->limit) / (float)frames;
    for (int i = 0; i < frames; i++) {
        float g = s->limit + dg * (float)i;
        for (int ch = 0; ch < 2; ch++) {
            float x = s->fx[2 * i + ch] * g, m = fabsf(x);
            if (m > 0.9f) x = copysignf(0.9f + 0.1f * tanhf((m - 0.9f) / 0.1f), x);
            if (s->channels == 2) out[2 * i + ch] = x;
            else if (ch == 0) out[i] = x * 0.5f;
            else out[i] += x * 0.5f;
        }
    }
    s->limit = target;
}

void sound_render(void *user, float *out, uint32_t frames)
{
    sound *s = user;
    double t0 = os_now();
    drain(s);
    int ch = s->channels;
    for (uint32_t done = 0; done < frames;) {
        int n = frames - done < CHUNK ? (int)(frames - done) : CHUNK;
        mix_chunk(s, out + (size_t)done * (size_t)ch, n);
        done += (uint32_t)n;
    }
    /* The mixer's share of real time, smoothed. */
    float load = frames ? (float)((os_now() - t0) * (double)s->rate / (double)frames) : 0.0f;
    uint32_t bits = atomic_load_explicit(&s->stat_load, memory_order_relaxed);
    float prev;
    memcpy(&prev, &bits, sizeof prev);
    prev += (load - prev) * 0.05f;
    memcpy(&bits, &prev, sizeof bits);
    atomic_store_explicit(&s->stat_load, bits, memory_order_relaxed);
}
