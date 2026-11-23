/* The simplex noise in noise.c: its gradient, its distribution (which the
 * generator's thresholds depend on), its isotropy, how seeds decorrelate,
 * the batched entry points against single points on both SIMD paths, and
 * that generated columns don't depend on the order they are made in, since
 * the generator memoises batched fields between columns. */
#include "block.h"
#include "noise.h"
#include "os.h"
#include "test_util.h"
#include "world.h"
#include "worldgen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLES 200000

static float g_buf[SAMPLES];

static int float_order(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Whether two floats have the same bits. */
static int bits_equal(float a, float b)
{
    uint32_t x = 0, y = 0;
    memcpy(&x, &a, sizeof x);
    memcpy(&y, &b, sizeof y);
    return x == y;
}

/* A spread of sample points that hits every part of a cell. */
static double px(int i)
{
    return i * 0.7311 + 0.1;
}

static double pz(int i)
{
    return (i % 997) * 1.913;
}

static double py(int i)
{
    return (i % 89) * 0.77;
}

/* The derivative noise2d_grad reports, against central differences of the
 * value it returns. eroded2d damps the finer octaves by this slope, so a
 * wrong factor would change the terrain without failing anything else. */
static void test_noise_gradient(void)
{
    double worst = 0.0, steepest = 0.0;
    int same = 1;
    for (int i = 0; i < 4000; i++) {
        double x = i * 0.377 - 700.0, z = i * 0.913 + 50.0, h = 1e-3;
        float dx = 0.0f, dz = 0.0f, v = noise2d_grad(3, x, z, &dx, &dz);
        same &= bits_equal(v, noise2d(3, x, z));
        double fx = (double)(noise2d(3, x + h, z) - noise2d(3, x - h, z)) / (2.0 * h);
        double fz = (double)(noise2d(3, x, z + h) - noise2d(3, x, z - h)) / (2.0 * h);
        worst = fmax(worst, fmax(fabs(fx - (double)dx), fabs(fz - (double)dz)));
        steepest = fmax(steepest, sqrt((double)(dx * dx + dz * dz)));
    }
    CHECK(same);
    CHECK(worst < 0.005);
    /* Measured 2.29 per block; much more would make cliffs out of hills. */
    CHECK(steepest > 1.5 && steepest < 2.8);
}

/* 3D noise has no reported gradient; check it is smooth instead: no jumps
 * between cells, which would show as seams in caves. */
static void test_noise3d_smooth(void)
{
    double worst = 0.0;
    for (int i = 0; i < 6000; i++) {
        double x = i * 0.613 - 900.0, y = (i % 113) * 0.71, z = i * 0.271 + 13.0, h = 1e-3;
        float v = noise3d(4, x, y, z);
        float a = noise3d(4, x + h, y, z), b = noise3d(4, x, y + h, z), c = noise3d(4, x, y, z + h);
        double d = fmax(fabs((double)(a - v)), fmax(fabs((double)(b - v)), fabs((double)(c - v)))) / h;
        worst = fmax(worst, d);
    }
    CHECK(worst < 4.0);
}

typedef struct {
    double sd_lo, sd_hi, q99_lo, q99_hi, q999_lo, q999_hi, max;
} spread;

/* Sorts g_buf and checks it against s: centred, odd (as many high as low),
 * and with the spread and tails the generator's thresholds were set for. */
static void check_spread(const spread *s)
{
    double mean = 0.0, var = 0.0;
    for (int i = 0; i < SAMPLES; i++) mean += (double)g_buf[i];
    mean /= SAMPLES;
    for (int i = 0; i < SAMPLES; i++) var += ((double)g_buf[i] - mean) * ((double)g_buf[i] - mean);
    double sd = sqrt(var / SAMPLES);
    qsort(g_buf, SAMPLES, sizeof g_buf[0], float_order);
    double q01 = (double)g_buf[SAMPLES / 100], q99 = (double)g_buf[SAMPLES * 99 / 100];
    double q999 = (double)g_buf[SAMPLES * 999 / 1000];
    CHECK(fabs(mean) < 0.01);
    CHECK(sd > s->sd_lo && sd < s->sd_hi);
    CHECK(q99 > s->q99_lo && q99 < s->q99_hi);
    CHECK(fabs(q99 + q01) < 0.02);
    CHECK(q999 > s->q999_lo && q999 < s->q999_hi);
    CHECK((double)g_buf[0] >= -s->max && (double)g_buf[SAMPLES - 1] <= s->max);
}

/* A plain simplex sum stops short of 0.56; the shaping gives back the tails
 * that lakes (0.62), cave chambers (0.55) and mouths depend on. */
static void test_noise_distribution(void)
{
    static const spread N2 = {0.29, 0.32, 0.62, 0.69, 0.76, 0.84, 1.0};
    static const spread N3 = {0.25, 0.29, 0.55, 0.61, 0.69, 0.76, 1.0};
    static const spread F4 = {0.17, 0.21, 0.39, 0.44, 0.50, 0.56, 1.0};
    static const spread E7 = {0.09, 0.115, 0.22, 0.27, 0.33, 0.40, 1.0};
    for (int i = 0; i < SAMPLES; i++) g_buf[i] = noise2d(3, px(i), pz(i));
    long lake = 0;
    for (int i = 0; i < SAMPLES; i++) lake += g_buf[i] > 0.62f;
    check_spread(&N2);
    for (int i = 0; i < SAMPLES; i++) g_buf[i] = noise3d(3, px(i), py(i), pz(i));
    long chamber = 0;
    for (int i = 0; i < SAMPLES; i++) chamber += g_buf[i] > 0.55f;
    check_spread(&N3);
    for (int i = 0; i < SAMPLES; i++) g_buf[i] = fbm2d(3, px(i), pz(i), 4);
    check_spread(&F4);
    for (int i = 0; i < SAMPLES; i++) g_buf[i] = eroded2d(3, px(i), pz(i), 7);
    check_spread(&E7);
    /* About 1.5 % of samples pass each threshold. */
    CHECK(lake > SAMPLES / 200 && lake < SAMPLES / 30);
    CHECK(chamber > SAMPLES / 200 && chamber < SAMPLES / 30);
}

/* Simplex has no preferred axis: a step along x, along z or along the
 * diagonal changes the value by the same amount on average. */
static void test_noise_isotropy(void)
{
    double ax = 0.0, az = 0.0, ad = 0.0;
    for (int i = 0; i < 40000; i++) {
        double x = (i % 200) * 3.1, z = (i / 200) * 3.7;
        double a = (double)noise2d(9, x, z);
        double bx = (double)noise2d(9, x + 1.0, z) - a, bz = (double)noise2d(9, x, z + 1.0) - a;
        double bd = (double)noise2d(9, x + M_SQRT1_2, z + M_SQRT1_2) - a;
        ax += bx * bx;
        az += bz * bz;
        ad += bd * bd;
    }
    CHECK(fabs(ax / az - 1.0) < 0.05 && fabs(ad / az - 1.0) < 0.05);

    double sx = 0.0, sy = 0.0, sz = 0.0, sd = 0.0, k = 0.57735026918962576;
    for (int i = 0; i < 40000; i++) {
        double x = (i % 200) * 3.1, z = (i / 200) * 3.7, y = (i % 37) * 2.3;
        double a = (double)noise3d(9, x, y, z);
        double bx = (double)noise3d(9, x + 1.0, y, z) - a, by = (double)noise3d(9, x, y + 1.0, z) - a;
        double bz = (double)noise3d(9, x, y, z + 1.0) - a, bd = (double)noise3d(9, x + k, y + k, z + k) - a;
        sx += bx * bx;
        sy += by * by;
        sz += bz * bz;
        sd += bd * bd;
    }
    CHECK(fabs(sx / sz - 1.0) < 0.07 && fabs(sy / sz - 1.0) < 0.07 && fabs(sd / sz - 1.0) < 0.07);
}

/* Fields with neighbouring seeds must not echo each other: rain and heat,
 * or two octaves, would line up into stripes. */
static void test_noise_seeds(void)
{
    double ab = 0.0, aa = 0.0, bb = 0.0, shift = 0.0;
    for (int i = 0; i < 40000; i++) {
        double x = (i % 200) * 3.1, z = (i / 200) * 3.7;
        double a = (double)noise2d(9, x, z), b = (double)noise2d(10, x, z);
        double c = (double)noise2d(9, x + 1.0, z);
        ab += a * b;
        aa += a * a;
        bb += b * b;
        shift += fabs(b - c);
    }
    CHECK(fabs(ab / sqrt(aa * bb)) < 0.03);
    /* A new seed is not the same field moved over by a block. */
    CHECK(shift / 40000.0 > 0.1);

    int same = 1, differ = 0;
    for (uint32_t s = 0; s < 64; s++) {
        float a = noise3d(s, 12.5, 3.25, -7.75), b = noise3d(s, 12.5, 3.25, -7.75);
        same &= bits_equal(a, b);
        differ += !bits_equal(a, noise3d(s + 1, 12.5, 3.25, -7.75));
    }
    CHECK(same && differ >= 62);
}

/* Lattice corners, half cells and coordinates far beyond the world's edge
 * all give finite values in range. */
static void test_noise_extremes(void)
{
    static const double AT[] = {0.0, 1.0, -1.0, 0.5, -0.5, 1e-9, -1e-9, 30000000.0, -30000000.0,
                                4294967296.0, -4294967296.0, 1e11, -1e11, 123456789.123, -98765.4321};
    int ok = 1;
    for (size_t i = 0; i < sizeof AT / sizeof AT[0]; i++)
        for (size_t j = 0; j < sizeof AT / sizeof AT[0]; j++) {
            float dx = 0.0f, dz = 0.0f;
            float a = noise2d_grad(1, AT[i], AT[j], &dx, &dz), b = noise3d(1, AT[i], AT[j], AT[i]);
            float c = fbm2d(1, AT[i], AT[j], 6), d = eroded2d(1, AT[j], AT[i], 7);
            ok &= isfinite(a) && isfinite(b) && isfinite(c) && isfinite(d) && isfinite(dx) && isfinite(dz);
            ok &= fabsf(a) <= 1.0f && fabsf(b) <= 1.0f && fabsf(c) <= 1.0f && fabsf(d) <= 1.0f;
        }
    CHECK(ok);
}

#define BATCH_N 37
#define PAD 4

/* Every batch length from 0 up, at every alignment, gives exactly the
 * single-point values, and writes nothing past its end. */
static int batch_matches(int n, int off)
{
    double x[BATCH_N + PAD] = {0}, y[BATCH_N + PAD] = {0}, z[BATCH_N + PAD] = {0};
    float v[BATCH_N + PAD] = {0}, dx[BATCH_N + PAD] = {0}, dz[BATCH_N + PAD] = {0};
    float w[BATCH_N + PAD] = {0}, f[BATCH_N + PAD] = {0}, e[BATCH_N + PAD] = {0};
    for (int i = 0; i < BATCH_N + PAD; i++) {
        /* Negative, on the lattice, between cells, and far away. */
        x[i] = (i % 3 == 0) ? (double)(i - 20) : i * -13.37 + (i % 5) * 1e9;
        y[i] = i * 0.5 - 3.0;
        z[i] = (i % 4 == 1) ? -0.5 * i : i * 7.77 - 2e8;
        v[i] = dx[i] = dz[i] = w[i] = f[i] = e[i] = 99.0f;
    }
    noise2d_grad_n(5, x + off, z + off, v + off, dx + off, dz + off, n);
    noise3d_n(5, x + off, y + off, z + off, w + off, n);
    fbm2d_n(5, x + off, z + off, 1 + n % 8, f + off, n);
    eroded2d_n(5, x + off, z + off, 1 + n % 8, e + off, n);
    int ok = 1;
    for (int i = 0; i < BATCH_N + PAD; i++) {
        if (i < off || i >= off + n) {
            ok &= v[i] == 99.0f && dx[i] == 99.0f && dz[i] == 99.0f;
            ok &= w[i] == 99.0f && f[i] == 99.0f && e[i] == 99.0f;
            continue;
        }
        float gx = 0.0f, gz = 0.0f, a = noise2d_grad(5, x[i], z[i], &gx, &gz);
        ok &= bits_equal(a, v[i]) && bits_equal(gx, dx[i]) && bits_equal(gz, dz[i]);
        ok &= bits_equal(noise3d(5, x[i], y[i], z[i]), w[i]);
        ok &= bits_equal(fbm2d(5, x[i], z[i], 1 + n % 8), f[i]);
        ok &= bits_equal(eroded2d(5, x[i], z[i], 1 + n % 8), e[i]);
    }
    return ok;
}

/* The gradient batch with only one of its outputs asked for. */
static int batch_one_gradient(void)
{
    double x[BATCH_N] = {0}, z[BATCH_N] = {0};
    float v[BATCH_N] = {0}, d[BATCH_N] = {0}, u[BATCH_N] = {0}, p[BATCH_N] = {0};
    for (int i = 0; i < BATCH_N; i++) {
        x[i] = i * 3.3 - 50.0;
        z[i] = i * -1.9 + 4.0;
    }
    noise2d_grad_n(8, x, z, v, d, NULL, BATCH_N);
    noise2d_grad_n(8, x, z, u, NULL, p, BATCH_N);
    int ok = 1;
    for (int i = 0; i < BATCH_N; i++) {
        float gx = 0.0f, gz = 0.0f, a = noise2d_grad(8, x[i], z[i], &gx, &gz);
        ok &= bits_equal(a, v[i]) && bits_equal(a, u[i]) && bits_equal(gx, d[i]) && bits_equal(gz, p[i]);
        ok &= bits_equal(a, noise2d(8, x[i], z[i]));
    }
    noise2d_n(8, x, z, p, BATCH_N);
    for (int i = 0; i < BATCH_N; i++) ok &= bits_equal(p[i], v[i]);
    return ok;
}

static void test_noise_batches(void)
{
    int avx2 = noise_avx2(1);
    CHECK(avx2 == 0 || avx2 == 1);
    CHECK(noise_avx2(1) == avx2);
#if !defined(__x86_64__)
    CHECK(avx2 == 0);
#endif
    for (int pass = 0; pass < 2; pass++) {
        /* Pass 0 forces the plain loops; pass 1 uses AVX2 where there is one. */
        CHECK(noise_avx2(pass) == (pass ? avx2 : 0));
        int ok = 1;
        for (int n = 0; n <= BATCH_N; n++)
            for (int off = 0; off < PAD && off + n <= BATCH_N + PAD; off++) ok &= batch_matches(n, off);
        CHECK(ok);
        CHECK(batch_one_gradient());
    }
    (void)noise_avx2(1);
}

/* A column's blocks, hashed. */
static uint64_t column_hash(uint32_t seed, int cx, int cz)
{
    column *c = column_alloc(cx, cz);
    worldgen_column(seed, c);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < COL_VOL; i++) h = (h ^ c->blocks[i]) * 1099511628211ull;
    column_free(c);
    return h;
}

/* The generator keeps batched slow fields and tree sites between columns.
 * Whatever order the columns come in (the workers take them as they are
 * streamed), and whether that memory is fresh or full of far-off columns,
 * each one comes out the same. */
static void test_column_order(void)
{
    static const int AT[][2] = {{0, 0}, {1, 0}, {0, 1}, {-1, -1}, {40, -3}, {-900, 512}, {41, -3}, {2, 2}, {-899, 511}};
    enum { COUNT = sizeof AT / sizeof AT[0] };
    uint64_t first[COUNT] = {0};
    worldgen_forget();
    for (int i = 0; i < COUNT; i++) first[i] = column_hash(77, AT[i][0], AT[i][1]);
    int same = 1;
    worldgen_forget();
    for (int i = COUNT - 1; i >= 0; i--) same &= column_hash(77, AT[i][0], AT[i][1]) == first[i];
    CHECK(same);
    same = 1;
    for (int i = 0; i < COUNT; i++) {
        /* Something far away in between, so the memory is of other land. */
        (void)column_hash(77, 3000 + i * 17, -2500 - i * 29);
        same &= column_hash(77, AT[i][0], AT[i][1]) == first[i];
    }
    CHECK(same);
    /* Another seed's columns, then this seed again. */
    (void)column_hash(78, 0, 0);
    CHECK(column_hash(77, 0, 0) == first[0]);
    worldgen_forget();
}

/* The two things the distribution fix brought back: lakes above the sea and
 * tall cave chambers. A plain simplex sum left 0.04 % of the land under
 * lakes and almost no air pocket taller than a corridor. */
static void test_lakes_and_chambers(void)
{
    for (uint32_t seed = 1; seed <= 3; seed++) {
        long wet = 0, land = 0;
        for (int i = 0; i < 6000; i++) {
            int x = i * 7919 % 60000 - 30000, z = i * 6151 % 60000 - 30000;
            worldgen_point p = worldgen_sample(seed, x, z);
            if (p.height < SEA_LEVEL && p.level <= SEA_LEVEL) continue;
            land++;
            wet += p.level > SEA_LEVEL;
        }
        /* Measured 1.9 to 2.2 % over seeds 1 to 3 and 1337. */
        CHECK(land > 3000 && wet * 100 > land && wet * 100 < land * 4);
    }

    long air = 0, rock = 0, tall = 0;
    for (int i = 0; i < 12; i++) {
        column *c = column_alloc(i * 89 % 1200 - 600, i * 157 % 1200 - 600);
        worldgen_column(1337, c);
        for (int z = 0; z < CHUNK_W; z++)
            for (int x = 0; x < CHUNK_W; x++) {
                int top = 0, run = 0;
                for (int y = WORLD_H - 1; y > 0 && !top; y--)
                    if (c->blocks[col_index(x, y, z)] != B_AIR) top = y;
                for (int y = 8; y < top - 6; y++) {
                    int open = c->blocks[col_index(x, y, z)] == B_AIR;
                    air += open;
                    rock += !open;
                    run = open ? run + 1 : 0;
                    tall += run == 6;
                }
            }
        column_free(c);
    }
    /* Measured 8.6 % of the underground open, and hundreds of open runs
     * six blocks tall in a dozen columns. */
    CHECK(air * 100 > (air + rock) * 4 && air * 100 < (air + rock) * 14);
    CHECK(tall > 200);
    worldgen_forget();
}

/* Octaves add detail without widening the field: fbm is normalised, so its
 * spread falls toward a floor as octaves are added, and eroded2d (which damps
 * the fine octaves on steep ground) stays flatter and gentler than fbm with
 * the same octaves. */
static void test_noise_octaves(void)
{
    double prev = 1.0;
    int falling = 1, gentler = 1, flatter = 1;
    for (int o = 1; o <= 8; o++) {
        double f2 = 0.0, e2 = 0.0, fs = 0.0, es = 0.0;
        for (int i = 0; i < 20000; i++) {
            double x = i * 0.7311, z = (i % 997) * 1.913;
            float f = fbm2d(5, x, z, o), e = eroded2d(5, x, z, o);
            f2 += (double)f * (double)f;
            e2 += (double)e * (double)e;
            fs += fabs((double)(fbm2d(5, x + 0.01, z, o) - f));
            es += fabs((double)(eroded2d(5, x + 0.01, z, o) - e));
        }
        double sd = sqrt(f2 / 20000.0);
        falling &= sd <= prev + 1e-3;
        flatter &= e2 < f2 * 0.5;
        gentler &= es < fs * 0.9;
        prev = sd;
    }
    CHECK(falling && flatter && gentler);
    /* Measured 0.178 with eight octaves: the floor, well above zero. */
    CHECK(prev > 0.16 && prev < 0.20);
}

/* worldgen_sample computes one point alone; worldgen_column computes a
 * whole column in batches. Spawn finding, thermo and the map read the
 * first; the blocks come from the second; they have to agree. */
static void test_sample_matches_column(void)
{
    long points = 0, ground = 0, wet = 0, water = 0, surface = 0;
    for (int i = 0; i < 8; i++) {
        int cx = i * 71 % 900 - 450, cz = i * 113 % 900 - 450;
        column *c = column_alloc(cx, cz);
        worldgen_column(1337, c);
        for (int z = 0; z < CHUNK_W; z++)
            for (int x = 0; x < CHUNK_W; x++) {
                worldgen_point p = worldgen_sample(1337, cx * CHUNK_W + x, cz * CHUNK_W + z);
                uint8_t below = c->blocks[col_index(x, p.height - 1, z)];
                points++;
                ground += below != B_AIR && below != B_WATER && below != B_ICE;
                if (p.level > 0) {
                    uint8_t w = c->blocks[col_index(x, p.level - 1, z)];
                    wet++;
                    water += w == B_WATER || w == B_ICE;
                    surface += c->blocks[col_index(x, p.level, z)] != B_WATER;
                }
            }
        column_free(c);
    }
    CHECK(points == 8 * COL_AREA && ground == points);
    CHECK(wet > 0 && water == wet && surface == wet);
    worldgen_forget();
}

/* The sea-level climate thermo starts from: in a plausible range, and
 * smooth, so walking a block never changes the air by more than a trace. */
static void test_sea_temp(void)
{
    double lo = 99.0, hi = -99.0, jump = 0.0;
    for (int i = 0; i < 6000; i++) {
        int x = i * 7919 % 60000 - 30000, z = i * 6151 % 60000 - 30000;
        float t = worldgen_sea_temp(1337, x, z);
        lo = fmin(lo, (double)t);
        hi = fmax(hi, (double)t);
        jump = fmax(jump, fabs((double)(worldgen_sea_temp(1337, x + 1, z) - t)));
        jump = fmax(jump, fabs((double)(worldgen_sea_temp(1337, x, z + 1) - t)));
    }
    /* Measured -3.9 to 28.4 C, at most 0.011 K per block. */
    CHECK(lo < 2.0 && lo > -15.0 && hi > 22.0 && hi < 36.0);
    CHECK(jump < 0.05);
}

/* Sixteen columns spread over a few hundred metres, hashed together. */
static uint64_t patch_hash(uint32_t seed)
{
    uint64_t h = 1469598103934665603ull;
    for (int z = 0; z < 4; z++)
        for (int x = 0; x < 4; x++) h = (h ^ column_hash(seed, x * 37 - 60, z * 53 - 80)) * 1099511628211ull;
    return h;
}

/* Golden terrain for more seeds than test_generators' 1337, on the plain
 * loops and on AVX2: a world must grow the same on every machine. */
static void test_golden_paths(void)
{
    static const uint64_t GOLD[3] = {0xddcc6b300c04143bull, 0xa258b306358e9e73ull, 0xcf75a14fac28fe44ull};
    int avx2 = noise_avx2(1);
    for (int pass = 0; pass < 2; pass++) {
        (void)noise_avx2(pass);
        for (uint32_t s = 1; s <= 3; s++) {
            worldgen_forget();
            CHECK(patch_hash(s) == GOLD[s - 1]);
        }
    }
    (void)noise_avx2(avx2);
    worldgen_forget();
}

#define THREADS 4
#define PER_THREAD 6

typedef struct {
    int first;
    uint64_t hash[PER_THREAD];
} gen_job;

static void column_of(int i, int *cx, int *cz)
{
    /* Neighbours share tree sites across their borders; the far ones fill
     * the shared site cache with other land. */
    *cx = i % 2 ? i / 2 : 300 + i * 41;
    *cz = i % 2 ? -(i / 4) : -200 - i * 23;
}

static void gen_thread(void *arg)
{
    gen_job *j = arg;
    for (int k = 0; k < PER_THREAD; k++) {
        int cx = 0, cz = 0;
        column_of((j->first + k * 5) % (THREADS * PER_THREAD), &cx, &cz);
        j->hash[k] = column_hash(91, cx, cz);
    }
}

/* Workers generate columns side by side, each with its own memo and all
 * sharing the tree-site cache. What they make matches one thread alone. */
static void test_threaded_columns(void)
{
    gen_job job[THREADS] = {0};
    os_thread t[THREADS] = {0};
    worldgen_forget();
    for (int i = 0; i < THREADS; i++) {
        job[i].first = i * 7;
        CHECK(os_thread_start(&t[i], gen_thread, &job[i]) == 0);
    }
    for (int i = 0; i < THREADS; i++) os_thread_join(t[i]);
    worldgen_forget();
    int same = 1;
    for (int i = 0; i < THREADS; i++)
        for (int k = 0; k < PER_THREAD; k++) {
            int cx = 0, cz = 0;
            column_of((job[i].first + k * 5) % (THREADS * PER_THREAD), &cx, &cz);
            same &= column_hash(91, cx, cz) == job[i].hash[k];
        }
    CHECK(same);
    worldgen_forget();
}

/* Each simplex corner picks its gradient from one hash_u32 of the seed and
 * the cell, so the mix has to be good: flipping any one input bit flips
 * about half the output bits (otherwise neighbouring cells share gradients
 * and the terrain shows a grid), and the low bits that choose the gradient
 * are evenly spread. */
static void test_hash_mix(void)
{
    double worst = 0.0;
    for (int bit = 0; bit < 32; bit++) {
        long flips = 0;
        for (uint32_t i = 0; i < 4000; i++) {
            uint32_t a = i * 2654435761u + 12345u;
            uint32_t d = hash_u32(a) ^ hash_u32(a ^ (1u << bit));
            for (int k = 0; k < 32; k++) flips += (d >> k) & 1u;
        }
        double share = (double)flips / (4000.0 * 32.0);
        worst = fmax(worst, fabs(share - 0.5));
    }
    CHECK(worst < 0.05);

    /* The 12 or 16 gradients come from the low 4 bits: count each value over
     * a block of neighbouring cells, as the noise walks them. */
    long count[16] = {0};
    for (int z = -64; z < 64; z++)
        for (int x = -64; x < 64; x++) count[hash2i(7, x, z) & 15u]++;
    double chi = 0.0, expect = 128.0 * 128.0 / 16.0;
    for (int k = 0; k < 16; k++) chi += ((double)count[k] - expect) * ((double)count[k] - expect) / expect;
    /* 15 degrees of freedom: 37.7 is the 0.1 % tail. */
    CHECK(chi < 37.7);

    /* Neighbouring cells and neighbouring seeds don't collide. */
    int same = 0;
    for (int x = -500; x < 500; x++) {
        same += hash2i(7, x, 0) == hash2i(7, x + 1, 0);
        same += hash2i(7, x, 0) == hash2i(7, 0, x) && x != 0;
        same += hash2i(7, x, 3) == hash2i(8, x, 3);
    }
    CHECK(same == 0);
}

/* K2 and K3 set the input frequency so the mean slope along an axis matches
 * the old noise's (0.7385 in 2D, 0.5770 in 3D): hills stay as steep and
 * caves as winding as the thresholds were tuned for. */
static void test_noise_slope(void)
{
    double s2 = 0.0, s3 = 0.0, sz = 0.0, h = 1e-3;
    for (int i = 0; i < 20000; i++) {
        double x = i * 0.7311, z = (i % 997) * 1.913, y = (i % 89) * 0.77;
        float dx = 0.0f, dz = 0.0f;
        (void)noise2d_grad(3, x, z, &dx, &dz);
        s2 += fabs((double)dx);
        sz += fabs((double)dz);
        double a = (double)noise3d(3, x, y, z);
        s3 += fabs((double)noise3d(3, x + h, y, z) - a) / h;
    }
    CHECK(s2 / 20000.0 > 0.71 && s2 / 20000.0 < 0.77);
    CHECK(s3 / 20000.0 > 0.55 && s3 / 20000.0 < 0.61);
    CHECK(fabs(sz / s2 - 1.0) < 0.05);
}

void test_noise_all(void)
{
    test_noise_gradient();
    test_noise3d_smooth();
    test_noise_distribution();
    test_noise_isotropy();
    test_noise_seeds();
    test_noise_extremes();
    test_noise_batches();
    test_column_order();
    test_lakes_and_chambers();
    test_noise_octaves();
    test_sample_matches_column();
    test_sea_temp();
    test_golden_paths();
    test_threaded_columns();
    test_hash_mix();
    test_noise_slope();
}
