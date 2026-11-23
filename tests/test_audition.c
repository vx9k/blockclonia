/* The listening tool (audition.c) and the exact takes it plays through the
 * mixer (sound_play_take): WAV encoding, the measurements, and a full
 * render into a temporary directory. */
#include "audition.h"
#include "mem.h"
#include "save.h"
#include "sound.h"
#include "sound_synth.h"
#include "test_util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int16_t sample_at(const uint8_t *wav, uint32_t i)
{
    return (int16_t)(uint16_t)(wav[44 + 2 * i] | wav[45 + 2 * i] << 8);
}

static void test_wav(void)
{
    const float in[6] = {0.0f, 1.0f, -1.0f, 0.5f, 3.0f, -7.0f};
    uint8_t *w = NULL;
    size_t len = audition_wav(in, 3, 48000, &w);
    CHECK(len == 44 + 12);
    CHECK(memcmp(w, "RIFF", 4) == 0 && memcmp(w + 8, "WAVEfmt ", 8) == 0 && memcmp(w + 36, "data", 4) == 0);
    CHECK(le32(w + 4) == len - 8);
    CHECK(le32(w + 16) == 16 && (le32(w + 20) & 0xffffu) == 1 && le32(w + 20) >> 16 == 2); /* PCM, stereo */
    CHECK(le32(w + 24) == 48000 && le32(w + 28) == 48000 * 4);
    CHECK((le32(w + 32) & 0xffffu) == 4 && le32(w + 32) >> 16 == 16); /* 4-byte frames of 16 bits */
    CHECK(le32(w + 40) == 12);
    CHECK(sample_at(w, 0) == 0 && sample_at(w, 1) == 32767 && sample_at(w, 2) == -32767);
    CHECK(sample_at(w, 3) == 16384 || sample_at(w, 3) == 16383);
    CHECK(sample_at(w, 4) == 32767 && sample_at(w, 5) == -32767); /* out of range clamps */
    mem_free(w);
}

static void test_measure(void)
{
    enum { N = 48000 };
    float *buf = mem_alloc(sizeof(float) * 2 * N);
    for (int i = 0; i < N; i++) buf[2 * i] = buf[2 * i + 1] = 0.5f * sinf(6.2831853f * 1000.0f * (float)i / 48000.0f);
    audition_stats st = {0};
    audition_measure(buf, N, 48000, &st);
    CHECK(fabsf(st.seconds - 1.0f) < 1e-6f);
    CHECK(fabsf(st.peak_db - -6.02f) < 0.1f);
    CHECK(fabsf(st.rms_db - -9.03f) < 0.1f); /* a sine's RMS is 3 dB under its peak */
    CHECK(fabsf(st.centroid_hz - 1000.0f) < 50.0f);
    for (int i = 0; i < N; i++) buf[2 * i] = buf[2 * i + 1] = 0.5f * sinf(6.2831853f * 4000.0f * (float)i / 48000.0f);
    audition_measure(buf, N, 48000, &st);
    CHECK(fabsf(st.centroid_hz - 4000.0f) < 100.0f);
    memset(buf, 0, sizeof(float) * 2 * N);
    audition_measure(buf, N, 48000, &st);
    CHECK(st.peak_db == AUDITION_SILENT_DB && st.rms_db == AUDITION_SILENT_DB && st.centroid_hz == 0.0f);
    audition_measure(buf, 0, 48000, &st); /* empty: no crash, all silent */
    CHECK(st.seconds == 0.0f && st.rms_db == AUDITION_SILENT_DB);

    /* One channel only: the peak is that channel's, the RMS is of the
     * mono mix (half the amplitude, 6 dB down). */
    for (int i = 0; i < N; i++) buf[2 * i] = 0.5f * sinf(6.2831853f * 1000.0f * (float)i / 48000.0f);
    audition_measure(buf, N, 48000, &st);
    CHECK(fabsf(st.peak_db - -6.02f) < 0.1f);
    CHECK(fabsf(st.rms_db - -15.05f) < 0.1f);
    CHECK(fabsf(st.centroid_hz - 1000.0f) < 50.0f);

    /* Shorter than one analysis block: zero-padded, still measured. */
    audition_measure(buf, 300, 48000, &st);
    CHECK(fabsf(st.seconds - 0.00625f) < 1e-6f);
    CHECK(st.centroid_hz > 500.0f && st.centroid_hz < 2500.0f);

    /* Two tones of equal power: the centroid sits between them. */
    for (int i = 0; i < N; i++) {
        const float t = (float)i / 48000.0f;
        buf[2 * i] = buf[2 * i + 1] = 0.25f * (sinf(6.2831853f * 500.0f * t) + sinf(6.2831853f * 3500.0f * t));
    }
    audition_measure(buf, N, 48000, &st);
    CHECK(fabsf(st.centroid_hz - 2000.0f) < 100.0f);

    /* Another rate: the same tone measures the same frequency. */
    for (int i = 0; i < N; i++) buf[2 * i] = buf[2 * i + 1] = 0.5f * sinf(6.2831853f * 1000.0f * (float)i / 24000.0f);
    audition_measure(buf, N, 24000, &st);
    CHECK(fabsf(st.seconds - 2.0f) < 1e-6f);
    CHECK(fabsf(st.centroid_hz - 1000.0f) < 50.0f);
    mem_free(buf);
}

static void test_take_len(void)
{
    sound *s = sound_create(7, SOUND_RATE, 2);
    CHECK(sound_take_len(s, SND_STEP, MAT_STONE, 0) > 0);
    CHECK(sound_take_len(s, SND_STEP, MAT_STONE, 2) > 0); /* steps have three takes */
    CHECK(sound_take_len(s, SND_UI_CLICK, MAT_GENERIC, 0) > 0);
    CHECK(sound_take_len(s, SND_UI_CLICK, MAT_GENERIC, 1) == 0); /* one take only */
    CHECK(sound_take_len(s, -1, MAT_STONE, 0) == 0 && sound_take_len(s, SND_COUNT, MAT_STONE, 0) == 0);
    CHECK(sound_take_len(s, SND_STEP, -1, 0) == 0 && sound_take_len(s, SND_STEP, MAT_COUNT, 0) == 0);
    CHECK(sound_take_len(s, SND_STEP, MAT_STONE, -1) == 0 &&
          sound_take_len(s, SND_STEP, MAT_STONE, SYNTH_VARIANTS) == 0);
    sound_destroy(s);
}

static int same_samples(const float *a, const float *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* Reads a comma-ended field of index.csv into out; returns the text after
 * the comma, or NULL. */
static const char *field(const char *p, char *out, size_t cap)
{
    const char *comma = p ? strchr(p, ',') : NULL;
    if (!comma || (size_t)(comma - p) >= cap) return NULL;
    memcpy(out, p, (size_t)(comma - p));
    out[comma - p] = '\0';
    return comma + 1;
}

/* Parses the four numbers after the names; returns 1 if all are there. */
static int numbers(const char *p, audition_stats *st)
{
    float *dst[4] = {&st->seconds, &st->peak_db, &st->rms_db, &st->centroid_hz};
    for (int i = 0; i < 4; i++) {
        char *end = NULL;
        *dst[i] = strtof(p, &end);
        if (end == p || *end != (i < 3 ? ',' : '\n')) return 0;
        p = end + 1;
    }
    return 1;
}

/* One take through a fresh mixer: its stats, and the samples in buf. */
static void play_one(int id, int mat, int var, const dvec3 *pos, int underwater, float *buf, int frames,
                     audition_stats *st)
{
    sound *s = sound_create(7, SOUND_RATE, 2);
    sound_listener(s, (dvec3){0.0, 0.0, 0.0}, 0.0f, 0.0f, underwater);
    sound_update(s, 0.0f);
    sound_render(s, buf, 9600); /* the underwater blend settles */
    sound_play_take(s, id, pos, mat, var, 1.0f);
    sound_render(s, buf, (uint32_t)frames);
    audition_measure(buf, (uint32_t)frames, SOUND_RATE, st);
    sound_destroy(s);
}

static void test_play_take(void)
{
    enum { N = 24000 };
    float *a = mem_alloc(sizeof(float) * 2 * N), *b = mem_alloc(sizeof(float) * 2 * N);
    audition_stats near = {0}, far = {0}, under = {0}, again = {0};
    const dvec3 at2 = {0.0, 0.0, -2.0}, at16 = {0.0, 0.0, -16.0};
    play_one(SND_BREAK, MAT_STONE, 0, &at2, 0, a, N, &near);
    play_one(SND_BREAK, MAT_STONE, 0, &at2, 0, b, N, &again);
    CHECK(near.rms_db > -40.0f);
    CHECK(same_samples(a, b, 2 * N)); /* the same take, sample for sample */
    play_one(SND_BREAK, MAT_STONE, 0, &at16, 0, b, N, &far);
    CHECK(far.rms_db < near.rms_db - 6.0f);
    play_one(SND_BREAK, MAT_STONE, 0, &at2, 1, b, N, &under);
    CHECK(under.centroid_hz < near.centroid_hz * 0.8f);
    /* A take with no clip of its own plays nothing, where sound_play
     * would fall back to another one. */
    audition_stats none = {0};
    play_one(SND_UI_CLICK, MAT_GENERIC, 1, NULL, 0, b, N, &none);
    CHECK(none.peak_db == AUDITION_SILENT_DB);
    play_one(SND_COUNT, MAT_GENERIC, 0, NULL, 0, b, N, &none);
    CHECK(none.peak_db == AUDITION_SILENT_DB);

    /* At the head (no position) it is centred: both channels the same. */
    audition_stats head = {0};
    play_one(SND_UI_CLICK, MAT_GENERIC, 0, NULL, 0, a, N, &head);
    CHECK(head.rms_db > -60.0f);
    int same = 1;
    for (int i = 0; i < N; i++) same &= a[2 * i] == a[2 * i + 1];
    CHECK(same);

    /* Intensity scales the take, and stops at 2 as in sound_play. */
    audition_stats loud = {0}, louder = {0};
    sound *s = sound_create(7, SOUND_RATE, 2);
    sound_play_take(s, SND_BREAK, &at16, MAT_STONE, 0, 0.0f); /* nothing to play */
    sound_render(s, a, 4800);
    audition_measure(a, 4800, SOUND_RATE, &none);
    CHECK(none.peak_db == AUDITION_SILENT_DB);
    sound_play_take(s, SND_BREAK, &at16, MAT_STONE, 0, 2.0f);
    sound_render(s, a, N);
    audition_measure(a, N, SOUND_RATE, &loud);
    sound_destroy(s);
    s = sound_create(7, SOUND_RATE, 2);
    sound_play_take(s, SND_BREAK, &at16, MAT_STONE, 0, 50.0f);
    sound_render(s, b, N);
    audition_measure(b, N, SOUND_RATE, &louder);
    sound_destroy(s);
    CHECK(loud.rms_db > far.rms_db + 5.0f);
    CHECK(same_samples(a, b, 2 * N));
    mem_free(a);
    mem_free(b);
}

/* The takes audition_render writes: every take dry, plus one take a sound
 * and the three loops in each of the other four settings. */
static int expected_files(void)
{
    sound *s = sound_create(1337, SOUND_RATE, 2);
    int all = 0;
    for (int id = 0; id < SND_COUNT; id++)
        for (int mat = 0; mat < MAT_COUNT; mat++)
            for (int var = 0; var < SYNTH_VARIANTS; var++) all += sound_take_len(s, id, mat, var) > 0;
    sound_destroy(s);
    return all + (int)LOOP_COUNT + 4 * ((int)SND_COUNT + (int)LOOP_COUNT);
}

/* One line of index.csv. */
typedef struct {
    char setting[32], take[64];
    audition_stats st;
} index_row;

#define MAX_ROWS 512
static index_row g_rows[MAX_ROWS];

/* Reads index.csv into g_rows: the number of takes, or -1 if malformed. */
static int read_index(const char *dir)
{
    char path[512] = {0};
    snprintf(path, sizeof path, "%s/index.csv", dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256] = {0};
    int n = 0;
    if (!fgets(line, sizeof line, f) || strcmp(line, "setting,take,seconds,peak_db,rms_db,centroid_hz\n") != 0) n = -1;
    while (n >= 0 && n < MAX_ROWS && fgets(line, sizeof line, f)) {
        index_row *r = &g_rows[n];
        const char *rest = field(field(line, r->setting, sizeof r->setting), r->take, sizeof r->take);
        if (!rest || !numbers(rest, &r->st)) {
            n = -1;
            break;
        }
        n++;
    }
    if (fclose(f) != 0) n = -1;
    return n;
}

static const index_row *find_row(int n, const char *setting, const char *take)
{
    for (int i = 0; i < n; i++)
        if (strcmp(g_rows[i].setting, setting) == 0 && strcmp(g_rows[i].take, take) == 0) return &g_rows[i];
    return NULL;
}

/* Left and right energy of a written WAV. Returns 0 if unreadable. */
static int wav_energy(const char *dir, const char *setting, const char *take, double *el, double *er)
{
    char path[512] = {0};
    snprintf(path, sizeof path, "%s/%s/%s.wav", dir, setting, take);
    const size_t cap = 4u << 20;
    uint8_t *w = mem_alloc(cap);
    long len = save_read_file(path, w, cap);
    *el = *er = 0.0;
    int ok = len > 44 && le32(w + 40) == (uint32_t)len - 44u;
    for (uint32_t i = 0; ok && i < le32(w + 40) / 4u; i++) {
        double l = (double)sample_at(w, 2 * i), r = (double)sample_at(w, 2 * i + 1);
        *el += l * l;
        *er += r * r;
    }
    mem_free(w);
    return ok;
}

/* What each setting does to the sounds, read back from index.csv and the
 * WAVs themselves. */
static void check_settings(const char *dir, int n)
{
    int silent = 0, pairs = 0, quieter = 0, duller = 0, right = 0, positional = 0;
    for (int i = 0; i < n; i++) {
        const index_row *r = &g_rows[i];
        if (r->st.rms_db <= -90.0f || !(r->st.seconds > 0.0f)) silent++;
        if (strcmp(r->setting, "near") != 0 || strncmp(r->take, "loop-", 5) == 0) continue;
        const index_row *far = find_row(n, "far", r->take), *under = find_row(n, "underwater", r->take);
        if (!far || !under) continue;
        pairs++;
        quieter += far->st.rms_db < r->st.rms_db - 3.0f;
        duller += under->st.centroid_hz < r->st.centroid_hz;
        double el = 0.0, er = 0.0;
        if (wav_energy(dir, "right", r->take, &el, &er)) {
            positional++;
            right += er > 2.0 * el;
        }
    }
    CHECK(silent == 0); /* every take, in every setting, can be heard */
    CHECK(pairs == SND_COUNT);
    CHECK(quieter == pairs);           /* 16 m away is quieter than 2 m for every sound */
    CHECK(duller >= pairs * 9 / 10);   /* under water muffles nearly everything */
    CHECK(positional == SND_COUNT && right == positional); /* "right" is heard on the right */

    /* The dry set has every material of a material sound, the others one. */
    static const char *const MATS[] = {"generic", "grass", "dirt",   "stone", "wood", "sand",
                                       "gravel",  "snow",  "leaves", "glass", "water"};
    char take[64] = {0};
    for (int m = 0; m < MAT_COUNT; m++) {
        snprintf(take, sizeof take, "step-%s-3", MATS[m]);
        CHECK(find_row(n, "dry", take) != NULL);
        CHECK(!find_row(n, "near", take));
    }
    CHECK(find_row(n, "near", "step-stone-1") && find_row(n, "dry", "ui-click-1") && !find_row(n, "dry", "ui-click-2"));
    /* The loops: four seconds each, in every setting. */
    static const char *const SETS[] = {"dry", "near", "right", "far", "underwater"};
    static const char *const LOOPS[] = {"loop-wind", "loop-water", "loop-fire"};
    for (int s = 0; s < 5; s++)
        for (int l = 0; l < 3; l++) {
            const index_row *r = find_row(n, SETS[s], LOOPS[l]);
            CHECK(r && fabsf(r->st.seconds - 4.0f) < 0.01f && r->st.peak_db <= 0.0f);
        }
    /* Wind is all around: as loud far as near. A fire is a place. */
    const index_row *wn = find_row(n, "near", "loop-wind"), *wf = find_row(n, "far", "loop-wind");
    const index_row *fn = find_row(n, "near", "loop-fire"), *ff = find_row(n, "far", "loop-fire");
    CHECK(wn && wf && fabsf(wn->st.rms_db - wf->st.rms_db) < 1.0f);
    CHECK(fn && ff && ff->st.rms_db < fn->st.rms_db - 6.0f);
}

static void remove_render(const char *dir, int n)
{
    static const char *const SUBDIRS[] = {"dry", "near", "right", "far", "underwater"};
    char path[512] = {0};
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s/%s.wav", dir, g_rows[i].setting, g_rows[i].take);
        tmp_remove(path);
    }
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, SUBDIRS[i]);
        tmp_remove(path);
    }
    snprintf(path, sizeof path, "%s/index.csv", dir);
    tmp_remove(path);
    tmp_remove(dir);
}

static void test_render(void)
{
    char a[256] = {0};
    if (!tmp_dir_make(a, sizeof a, "mc_audition_")) {
        CHECK(0);
        return;
    }
    /* One render (a few seconds); test_play_take covers determinism. */
    const int want = expected_files();
    CHECK(audition_render(a, 1337) == want);
    int n = read_index(a);
    CHECK(n == want && n < MAX_ROWS);
    check_settings(a, n);

    /* A file as written: a WAV whose data is the rest of the file. */
    char pa[512] = {0};
    snprintf(pa, sizeof pa, "%s/far/shot-1.wav", a);
    const size_t cap = 4u << 20;
    uint8_t *da = mem_alloc(cap);
    long la = save_read_file(pa, da, cap);
    CHECK(la > 44 && memcmp(da, "RIFF", 4) == 0 && le32(da + 40) == (uint32_t)la - 44u);
    mem_free(da);
    remove_render(a, n);

    /* A directory that can't be made fails cleanly. */
    if (tmp_dir_make(a, sizeof a, "mc_audition_")) {
        char blocked[300] = {0};
        snprintf(pa, sizeof pa, "%s/file", a);
        CHECK(save_write_file(pa, (const uint8_t *)"x", 1) == 0);
        snprintf(blocked, sizeof blocked, "%s/file/sub", a);
        CHECK(audition_render(blocked, 1) == -1);
        tmp_remove(pa);
        tmp_remove(a);
    }
}

void test_audition_all(void)
{
    test_wav();
    test_measure();
    test_take_len();
    test_play_take();
    test_render();
}
