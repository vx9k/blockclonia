/* Visible bleeding (blood.c): every millilitre bled is somewhere, as a
 * drop or a stain; venous blood drips and arterial blood streams out,
 * surging with each beat as far as the pressure throws it; stains land on
 * any face, pool, dry and, minutes later, clean themselves up; and it all
 * looks the same at 30 and 144 fps. */
#include "avatar.h"
#include "blood.h"
#include "health.h"
#include "mem.h"
#include "sysinfo.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

#define RHO 1060.0f /* kg/m3, whole blood */
#define MMHG 133.322f

static float stained(const blood_state *b, int *count)
{
    float ml = 0.0f;
    int n = 0;
    for (int i = 0; i < b->max_stains; i++)
        if (b->stain[i].live) {
            ml += b->stain[i].ml;
            n++;
        }
    if (count) *count = n;
    return ml;
}

static float flying(const blood_state *b)
{
    float ml = 0.0f;
    for (int i = 0; i < b->drops; i++) ml += b->drop[i].ml;
    return ml;
}

/* A venous wound 1 m above the flat test world's ground, facing +x. */
static blood_source venous(float ml_min)
{
    blood_source s = {dv3(0.5, GROUND + 1.0, 0.5), dv3(1.0, 0.0, 0.0), ml_min, 0, 0.0f};
    return s;
}

/* Arterial pressure t seconds in with the heart beating at hr around a
 * mean of p: a systolic surge over the first 0.3 s of each beat. */
static float pulse(float p, float hr, float t)
{
    if (hr <= 0.0f) return p;
    float phase = fmodf(t * hr / 60.0f, 1.0f);
    return p * (0.8f + (phase < 0.3f ? 0.6f * sinf(3.14159265f * phase / 0.3f) : 0.0f));
}

/* Bleeds s for `seconds` at fps (pressure pulsing around p_mean at hr),
 * then lets the drops settle for `settle` seconds. */
static void bleed_for(blood_state *b, const world *w, blood_source s, float p_mean, float hr, float seconds, float fps,
                      float settle)
{
    float dt = 1.0f / fps;
    int steps = (int)lroundf(seconds * fps), rest = (int)lroundf(settle * fps);
    for (int i = 0; i < steps + rest; i++) {
        blood_source now = s;
        if (i >= steps) now.ml_min = 0.0f;
        blood_bleed(b, &now, 1, pulse(p_mean, hr, (float)i * dt), p_mean, 0.98f, dv3(0, 0, 0), dt);
        blood_step(b, w, dt);
    }
}

/* What a source has bled that is not yet out as drops. */
static float pending(const blood_state *b, int i) { return b->owed[i] + b->ready[i]; }

static void test_blood_drips(void)
{
    test_world t;
    tw_init(&t);
    blood_state b;
    blood_init(&b, 1024, 7u);
    /* 60 mL/min for 10 s is 10 mL, all of it accounted for: in the air,
     * on the ground, or still owed for the next drop. */
    bleed_for(&b, &t.w, venous(60.0f), 120.0f, 0.0f, 10.0f, 60.0f, 0.0f);
    float all = stained(&b, NULL) + flying(&b) + pending(&b, 0);
    CHECK(fabsf(all - 10.0f) < 0.01f);
    CHECK(b.owed[0] < BLOOD_DROP_ML + 1e-4f);
    /* After a few seconds it has all landed, on the ground's top faces,
     * near the wound and below it, dark red. */
    bleed_for(&b, &t.w, venous(0.0f), 120.0f, 0.0f, 0.0f, 60.0f, 3.0f);
    int n = 0;
    CHECK(b.drops == 0 && fabsf(stained(&b, &n) + pending(&b, 0) - 10.0f) < 0.01f && n > 0);
    for (int i = 0; i < b.max_stains; i++) {
        const blood_stain *st = &b.stain[i];
        if (!st->live) continue;
        CHECK(st->face == 3 && fabs(st->pos.y - GROUND) < 0.01 && st->shade < 64);
        CHECK(fabs(st->pos.x - 0.5) < 0.5 && fabs(st->pos.z - 0.5) < 0.5);
    }
    blood_free(&b);
    tw_free(&t);
}

static void test_blood_fps(void)
{
    /* The same bleeding stains the same amount at 30 and 144 fps, and an
     * artery throws its blood about as far. */
    test_world t;
    tw_init(&t);
    float ml[2] = {0}, reach[2] = {0};
    const float fps[2] = {30.0f, 144.0f};
    for (int k = 0; k < 2; k++) {
        blood_state b;
        blood_init(&b, 4096, 11u);
        blood_source s = venous(300.0f);
        s.arterial = 1;
        bleed_for(&b, &t.w, s, 120.0f, 90.0f, 6.0f, fps[k], 3.0f);
        ml[k] = stained(&b, NULL);
        for (int i = 0; i < b.max_stains; i++)
            if (b.stain[i].live) reach[k] += b.stain[i].ml * (float)(b.stain[i].pos.x - 0.5);
        reach[k] /= ml[k];
        blood_free(&b);
    }
    CHECK(ml[0] > 25.0f && fabsf(ml[0] - ml[1]) < 0.03f * ml[0]);
    CHECK(fabsf(reach[0] - reach[1]) < 0.1f * reach[0]);
    tw_free(&t);
}

/* Blood an artery at 300 mL/min puts out over `frames` frames at 60 fps
 * with the pressure at p_now (mean p_mean), without letting it land. */
static float stream(blood_state *b, blood_source s, float p_now, float p_mean, int frames, int *frames_with_drops)
{
    float before = flying(b);
    int with = 0;
    for (int i = 0; i < frames; i++) {
        int had = b->drops;
        blood_bleed(b, &s, 1, p_now, p_mean, 0.98f, dv3(0, 0, 0), 1.0f / 60.0f);
        with += b->drops > had;
    }
    if (frames_with_drops) *frames_with_drops = with;
    return flying(b) - before;
}

static void test_blood_stream(void)
{
    test_world t;
    tw_init(&t);
    blood_state b;
    blood_init(&b, 1024, 3u);
    blood_source s = venous(300.0f);
    s.arterial = 1;
    /* An artery doesn't hold its blood for the next beat: it streams out
     * every frame, a continuous jet rather than a burst of spray. */
    int with = 0;
    stream(&b, s, 100.0f, 100.0f, 60, &with);
    CHECK(with >= 50);
    CHECK(fabsf(flying(&b) + pending(&b, 0) - 5.0f) < 0.01f);
    /* As fast as the pressure drives it through the wound (Bernoulli,
     * less the wound's losses), in a tight jet along the way the wound
     * faces. */
    float v = BLOOD_JET * sqrtf(2.0f * 100.0f * MMHG / RHO), along = 0.0f, side = 0.0f;
    for (int i = 0; i < b.drops; i++) {
        along += (float)b.drop[i].vel.x / (float)b.drops;
        side = fmaxf(side, fabsf((float)b.drop[i].vel.z));
    }
    CHECK(fabsf(along - v) < 0.1f * v && side < 0.1f * v);
    for (int i = 0; i < b.drops; i++) CHECK(b.drop[i].shade > 200); /* bright, oxygenated */
    /* Each beat's surge throws more blood, and faster, than the lull
     * between beats. */
    float surge = stream(&b, s, 140.0f, 100.0f, 18, NULL);
    int last = b.drops;
    float lull = stream(&b, s, 70.0f, 100.0f, 18, NULL);
    float v_lull = 0.0f;
    for (int i = last; i < b.drops; i++) v_lull += (float)b.drop[i].vel.x / (float)(b.drops - last);
    CHECK(surge > 1.6f * lull && v_lull < 0.9f * v);
    blood_free(&b);

    /* Falling pressure shortens the jet: at half the pressure the blood
     * lands much nearer. */
    float reach[2] = {0};
    const float p[2] = {120.0f, 60.0f};
    for (int k = 0; k < 2; k++) {
        blood_init(&b, 4096, 5u);
        bleed_for(&b, &t.w, s, p[k], 100.0f, 3.0f, 60.0f, 3.0f);
        float ml = stained(&b, NULL);
        for (int i = 0; i < b.max_stains; i++)
            if (b.stain[i].live) reach[k] += b.stain[i].ml * (float)(b.stain[i].pos.x - 0.5);
        reach[k] /= ml;
        blood_free(&b);
    }
    CHECK(reach[0] > 1.0f && reach[1] < 0.8f * reach[0]);
    /* The column above a wound adds to the heart's pressure: 50 from the
     * heart and 50 from the column throw as fast as 100 from the heart. */
    blood_init(&b, 1024, 3u);
    blood_source low = s;
    low.p_column = 50.0f;
    stream(&b, low, 50.0f, 50.0f, 30, NULL);
    along = 0.0f;
    for (int i = 0; i < b.drops; i++) along += (float)b.drop[i].vel.x / (float)b.drops;
    CHECK(b.drops > 10 && fabsf(along - v) < 0.1f * v);
    blood_free(&b);
    /* With no pressure behind it (an arrested heart) an artery only wells
     * up and drips. */
    blood_init(&b, 1024, 3u);
    stream(&b, s, 4.0f, 4.0f, 60, NULL);
    CHECK(b.drops > 10);
    for (int i = 0; i < b.drops; i++) CHECK(fabs(b.drop[i].vel.x) < 0.2);
    blood_free(&b);
    /* Low oxygen darkens arterial blood toward venous red. */
    blood_init(&b, 64, 3u);
    blood_bleed(&b, &s, 1, 100.0f, 100.0f, 0.6f, dv3(0, 0, 0), 1.0f);
    CHECK(b.drops > 0 && b.drop[0].shade < 160);
    blood_free(&b);
    tw_free(&t);
}

static void test_blood_walls(void)
{
    /* A jet at a wall stains the wall's face toward the wound. */
    test_world t;
    tw_init(&t);
    for (int y = GROUND; y < GROUND + 4; y++)
        for (int z = -3; z <= 3; z++) world_set(&t.w, 1, y, z, B_STONE, 0);
    blood_state b;
    blood_init(&b, 1024, 9u);
    blood_source s = venous(300.0f);
    s.arterial = 1;
    bleed_for(&b, &t.w, s, 120.0f, 100.0f, 2.0f, 60.0f, 3.0f);
    int wall = 0, total = 0;
    for (int i = 0; i < b.max_stains; i++) {
        if (!b.stain[i].live) continue;
        total++;
        if (b.stain[i].face == 0) {
            wall++;
            CHECK(fabs(b.stain[i].pos.x - 1.0) < 0.01);
        }
    }
    CHECK(wall > 0 && wall * 2 > total);
    blood_free(&b);
    tw_free(&t);
}

static void test_blood_merge(void)
{
    /* Drops falling on one spot pool into one growing stain, not a pile
     * of them. */
    test_world t;
    tw_init(&t);
    blood_state b;
    blood_init(&b, 1024, 13u);
    blood_source s = {dv3(0.5, GROUND + 0.3, 0.5), dv3(0.0, -1.0, 0.0), 30.0f, 0, 0.0f};
    bleed_for(&b, &t.w, s, 120.0f, 0.0f, 10.0f, 60.0f, 2.0f);
    int n = 0;
    float ml = stained(&b, &n), most = 0.0f;
    for (int i = 0; i < b.max_stains; i++)
        if (b.stain[i].live) most = fmaxf(most, b.stain[i].ml);
    CHECK(ml > 4.5f && n <= 4 && most > 0.5f * ml);
    blood_free(&b);
    tw_free(&t);
}

static void test_blood_pool(void)
{
    /* A long bleed onto one spot spreads as a pool of irregular lobes
     * around it, not one ever-growing round stain. */
    test_world t;
    tw_init(&t);
    blood_state b;
    blood_init(&b, 1024, 31u);
    blood_source s = {dv3(0.5, GROUND + 0.3, 0.5), dv3(0.0, -1.0, 0.0), 120.0f, 0, 0.0f};
    bleed_for(&b, &t.w, s, 120.0f, 0.0f, 20.0f, 60.0f, 2.0f);
    int n = 0;
    float ml = stained(&b, &n);
    CHECK(ml > 39.0f && n >= 5);
    double far = 0.0, edge[4] = {0}; /* how far the pool reaches toward +x, -x, +z and -z */
    for (int i = 0; i < b.max_stains; i++) {
        const blood_stain *st = &b.stain[i];
        if (!st->live) continue;
        float r = sqrtf(st->ml * 1e-6f / 0.001f / 3.14159265f);
        CHECK(r <= BLOOD_LOBE_R + 1e-4f);
        far = fmax(far, hypot(st->pos.x - 0.5, st->pos.z - 0.5));
        const double off[4] = {st->pos.x - 0.5, 0.5 - st->pos.x, st->pos.z - 0.5, 0.5 - st->pos.z};
        for (int e = 0; e < 4; e++) edge[e] = fmax(edge[e], off[e] + (double)r);
    }
    /* It reaches past one lobe, and lopsidedly: further one way than
     * another. */
    double most = fmax(fmax(edge[0], edge[1]), fmax(edge[2], edge[3]));
    double least = fmin(fmin(edge[0], edge[1]), fmin(edge[2], edge[3]));
    CHECK(far > (double)BLOOD_LOBE_R && most > 1.2 * least);
    blood_free(&b);

    /* A drop hitting fast splashes small spots around where it hit,
     * thrown the way it was going; a slow one just lands. Not a drop of
     * blood is lost either way. */
    const float speed[2] = {6.0f, 1.0f};
    for (int k = 0; k < 2; k++) {
        blood_init(&b, 1024, 37u);
        b.drops = 1;
        b.drop[0] = (blood_drop){dv3(0.5, GROUND + 0.05, 0.5),
                                 dv3(0.6 * (double)speed[k], -0.8 * (double)speed[k], 0.0), 0.2f, 40};
        blood_step(&b, &t.w, 0.1f);
        ml = stained(&b, &n);
        CHECK(b.drops == 0 && fabsf(ml - 0.2f) < 1e-4f);
        if (k == 0) {
            int ahead = 0;
            for (int i = 0; i < b.max_stains; i++) ahead += b.stain[i].live && b.stain[i].pos.x > 0.56;
            CHECK(n >= 3 && ahead >= 1);
        } else {
            CHECK(n == 1);
        }
        blood_free(&b);
    }
    tw_free(&t);
}

static void test_blood_ages(void)
{
    test_world t;
    tw_init(&t);
    blood_state b;
    blood_init(&b, 1024, 17u);
    bleed_for(&b, &t.w, venous(60.0f), 120.0f, 0.0f, 2.0f, 60.0f, 2.0f);
    int n = 0;
    stained(&b, &n);
    CHECK(n > 0);
    /* Blood stays where it fell for minutes, long enough to leave a pool
     * or a trail: still there at BLOOD_STAIN_S less a few seconds, then
     * gone. */
    CHECK(BLOOD_STAIN_S >= 300.0f && BLOOD_DRY_S < BLOOD_STAIN_S);
    bleed_for(&b, &t.w, venous(0.0f), 120.0f, 0.0f, 0.0f, 10.0f, BLOOD_STAIN_S - 6.0f);
    stained(&b, &n);
    CHECK(n > 0);
    bleed_for(&b, &t.w, venous(0.0f), 120.0f, 0.0f, 0.0f, 10.0f, 8.0f);
    stained(&b, &n);
    CHECK(n == 0);
    /* Wet blood is red, scarlet from an artery; dried, it is darker and
     * browner. */
    uint32_t art = blood_colour(255, 0.0f), ven = blood_colour(0, 0.0f), dry = blood_colour(255, BLOOD_DRY_S);
    CHECK((art >> 16) > (ven >> 16) && (art >> 16) > 3u * (art >> 8 & 255u));
    CHECK((dry >> 16) < (art >> 16) && (dry >> 8 & 255u) * 4u > (dry >> 16));
    blood_free(&b);
    tw_free(&t);
}

static void test_blood_water(void)
{
    test_world t;
    tw_init(&t);
    blood_state b;
    blood_init(&b, 1024, 19u);
    /* Drops falling into water are gone; no stain under it. */
    for (int x = -2; x <= 2; x++)
        for (int z = -2; z <= 2; z++) world_set(&t.w, x, GROUND, z, B_WATER, 0);
    blood_source s = {dv3(0.5, GROUND + 1.5, 0.5), dv3(0.0, -1.0, 0.0), 60.0f, 0, 0.0f};
    bleed_for(&b, &t.w, s, 120.0f, 0.0f, 2.0f, 60.0f, 2.0f);
    int n = 0;
    stained(&b, &n);
    CHECK(n == 0 && b.drops == 0);
    /* Stains that water covers later wash off within a few seconds. */
    for (int x = -2; x <= 2; x++)
        for (int z = -2; z <= 2; z++) world_set(&t.w, x, GROUND, z, B_AIR, 0);
    bleed_for(&b, &t.w, s, 120.0f, 0.0f, 2.0f, 60.0f, 2.0f);
    stained(&b, &n);
    CHECK(n > 0);
    for (int x = -2; x <= 2; x++)
        for (int z = -2; z <= 2; z++) world_set(&t.w, x, GROUND, z, B_WATER, 0);
    bleed_for(&b, &t.w, venous(0.0f), 120.0f, 0.0f, 0.0f, 60.0f, 4.0f);
    stained(&b, &n);
    CHECK(n == 0);
    blood_free(&b);
    tw_free(&t);
}

static void test_blood_ring(void)
{
    /* The stains are a ring of the machine's budget: past it, the oldest
     * go first. */
    test_world t;
    tw_init(&t);
    blood_state b;
    blood_init(&b, 16, 23u);
    blood_source s = venous(300.0f);
    s.arterial = 1;
    bleed_for(&b, &t.w, s, 120.0f, 100.0f, 4.0f, 60.0f, 3.0f);
    int n = 0;
    stained(&b, &n);
    CHECK(n == 16 && b.max_stains == 16);
    blood_free(&b);
    CHECK(b.stain == NULL && b.max_stains == 0);
    /* The budget grows with the machine and is never tiny. */
    sys_budget small = {0}, big = {0}, none = {0};
    const sys_info pi = {.ram = 4ull << 30, .ram_avail = 3ull << 30, .cores = 4};
    const sys_info desk = {.ram = 32ull << 30, .ram_avail = 24ull << 30, .cores = 16};
    const sys_info unknown = {0};
    sys_plan(&pi, &small);
    sys_plan(&desk, &big);
    sys_plan(&unknown, &none);
    CHECK(small.blood_stains >= 256 && big.blood_stains > small.blood_stains && none.blood_stains >= 256);
    CHECK(big.blood_stains <= 1024);
    tw_free(&t);
}

static void test_blood_build(void)
{
    test_world t;
    tw_init(&t);
    blood_state b;
    blood_init(&b, 1024, 29u);
    bleed_for(&b, &t.w, venous(120.0f), 120.0f, 0.0f, 1.0f, 60.0f, 0.2f);
    int n = 0;
    stained(&b, &n);
    CHECK(n > 0 && b.drops > 0);
    entity_instance out[2048] = {0};
    const dvec3 eye = dv3(0.5, GROUND + 1.6, 3.0);
    int k = blood_build(&b, &t.w, NULL, eye, 1.0f, out, 2048);
    CHECK(k == 4 * n + b.drops); /* a stain is four boxes making a regular octagon */
    /* Stains are flat on their face, thinner than a millimetre or two,
     * just off it so the block doesn't hide them, and the colour of
     * blood. */
    int flat = 0;
    for (int i = 0; i < k; i++) {
        CHECK(out[i].scale[0] > 0.0f && out[i].scale[1] > 0.0f && out[i].scale[2] > 0.0f);
        CHECK(fabsf(out[i].pos[2] + 2.5f) < 1.5f); /* relative to the eye */
        if (out[i].scale[1] < 0.003f) {
            flat++;
            /* Each box spans the octagon across a pair of opposite sides
             * and is as wide as one side, so the four together are the
             * octagon exactly, with no star points. */
            float lo = fminf(out[i].scale[0], out[i].scale[2]), hi = fmaxf(out[i].scale[0], out[i].scale[2]);
            CHECK(fabsf(lo / hi - 0.41421f) < 0.01f);
            CHECK(out[i].pos[1] + 1.6f > 0.0f && out[i].pos[1] + 1.6f < 0.003f);
        }
    }
    CHECK(flat == 4 * n);
    /* The renderer works in linear light and writes sRGB, so the tint is
     * the stain's colour in linear terms: dark venous red has almost no
     * green left (sRGB 8 is linear 0.2%). */
    for (int i = 0; i < k; i++) {
        uint32_t c = ~out[i].tint;
        CHECK((c & 255u) > 4u * ((c >> 8) & 255u) && ((c >> 8) & 255u) < 3u && (c & 255u) < 0x60u);
    }
    CHECK(blood_build(&b, &t.w, NULL, eye, 1.0f, out, 3) == 3);
    /* Short of room, the falling drops still get drawn: a stream in the
     * air matters more than the oldest stain. */
    k = blood_build(&b, &t.w, NULL, eye, 1.0f, out, b.drops + 5);
    int air = 0;
    for (int i = 0; i < k; i++) air += out[i].scale[1] >= 0.003f;
    CHECK(k == b.drops + 4 && air == b.drops);
    /* A falling drop is drawn streaked along its path, so a stream reads
     * as a stream and not a row of beads. */
    b.drops = 1;
    b.drop[0].vel = dv3(3.0, 0.0, 0.0);
    k = blood_build(&b, &t.w, NULL, eye, 1.0f, out, 2048);
    const entity_instance *d = &out[k - 1];
    const float zax[3] = {0.0f, 0.0f, 1.0f};
    float along[3] = {0};
    quat_apply((quat){d->rot[0], d->rot[1], d->rot[2], d->rot[3]}, zax, along);
    CHECK(d->scale[2] > 4.0f * d->scale[0] && fabsf(along[0]) > 0.99f);
    /* A stain near the end of its time shrinks away rather than blinking
     * out. */
    float fresh = out[0].scale[0];
    for (int i = 0; i < b.max_stains; i++) b.stain[i].age = BLOOD_STAIN_S - 5.0f;
    blood_build(&b, &t.w, NULL, eye, 1.0f, out, 2048);
    CHECK(out[0].scale[0] < 0.5f * fresh && out[0].scale[0] > 0.0f);
    CHECK(blood_build(&b, &t.w, NULL, eye, 1.0f, NULL, 3) == 0);
    blood_free(&b);
    tw_free(&t);
}

/* The still standing figure, facing yaw. */
static avatar_pose standing(float yaw)
{
    avatar_anim a = {0};
    avatar_pose p = {0};
    avatar_pose_get(&a, yaw, 0.0f, 0, &p);
    return p;
}

static void test_blood_spots(void)
{
    const avatar_pose st = standing(0.0f), quarter = standing(1.5707963f);
    /* Wounds open where the parts are: the head highest, the legs lowest,
     * the right arm on the right; the way a wound faces is a unit vector
     * turning with the body. */
    dvec3 at[BP_COUNT], out[BP_COUNT];
    for (int p = 0; p < BP_COUNT; p++) {
        blood_spot(p, 0, &st, dv3(10.0, 20.0, 30.0), &at[p], &out[p]);
        CHECK(fabs(dv3_len(out[p]) - 1.0) < 1e-4);
    }
    CHECK(at[BP_HEAD].y > at[BP_CHEST].y && at[BP_CHEST].y > at[BP_ABDOMEN].y && at[BP_ABDOMEN].y > at[BP_LLEG].y);
    CHECK(at[BP_HEAD].y < 20.0 + 1.8 && at[BP_RLEG].y > 20.0);
    CHECK(at[BP_RARM].x > 10.1 && at[BP_LARM].x < 9.9 && out[BP_RARM].x > 0.5);
    CHECK(out[BP_CHEST].z < -0.5); /* yaw 0 faces -z */
    dvec3 a2, o2;
    blood_spot(BP_RARM, 0, &quarter, dv3(10.0, 20.0, 30.0), &a2, &o2);
    CHECK(a2.z > 30.1 && o2.z > 0.5); /* a quarter turn: the right is +z */
    blood_spot(BP_CHEST, 1, &st, dv3(10.0, 20.0, 30.0), &a2, &o2);
    CHECK(dv3_len(dv3(a2.x - at[BP_CHEST].x, a2.y - at[BP_CHEST].y, a2.z - at[BP_CHEST].z)) > 0.02);
    /* Several wounds in one part open at different places around it and
     * bleed different ways, so four cut arteries in a leg are four jets,
     * not one thick one; and each stays on the leg's skin. */
    dvec3 la[4], lo[4];
    for (int k = 0; k < 4; k++) {
        blood_spot(BP_RLEG, k, &st, dv3(10.0, 20.0, 30.0), &la[k], &lo[k]);
        CHECK(fabs(dv3_len(lo[k]) - 1.0) < 1e-4 && lo[k].z < 0.3); /* none bleeds back into the body's front */
        CHECK(la[k].y > 20.1 && la[k].y < 20.9 && fabs(la[k].x - 10.1) < 0.12 && fabs(la[k].z - 30.0) < 0.12);
        for (int j = 0; j < k; j++) {
            double c = lo[k].x * lo[j].x + lo[k].y * lo[j].y + lo[k].z * lo[j].z;
            CHECK(c < 0.9); /* more than 25 degrees apart */
            CHECK(dv3_len(dv3(la[k].x - la[j].x, la[k].y - la[j].y, la[k].z - la[j].z)) > 0.03);
        }
    }
    /* Below the head, wounds bleed somewhat downward, not up like a
     * fountain. */
    for (int p = BP_CHEST; p < BP_COUNT; p++)
        for (int k = 0; k < 8; k++) {
            dvec3 a3, o3;
            blood_spot(p, k, &st, dv3(10.0, 20.0, 30.0), &a3, &o3);
            CHECK(o3.y < -0.1 && o3.y > -0.6);
        }
}

static void test_blood_garbage(void)
{
    test_world t;
    tw_init(&t);
    blood_state b;
    blood_init(&b, 64, 31u);
    blood_source s[BLOOD_SOURCES + 2];
    for (int i = 0; i < BLOOD_SOURCES + 2; i++) s[i] = venous(NAN);
    s[1].at.x = (double)NAN;
    s[2].ml_min = INFINITY;
    s[3].ml_min = -5.0f;
    s[4].out = dv3(0, 0, 0);
    s[4].ml_min = 60.0f;
    blood_bleed(&b, s, BLOOD_SOURCES + 2, NAN, NAN, NAN, dv3((double)NAN, 0, 0), 1.0f / 60.0f);
    blood_bleed(&b, s, BLOOD_SOURCES + 2, INFINITY, 0.0f, 0.98f, dv3(0, 0, 0), NAN);
    blood_step(&b, &t.w, NAN);
    blood_step(&b, &t.w, -1.0f);
    for (int i = 0; i < 120; i++) {
        blood_bleed(&b, s, BLOOD_SOURCES + 2, 120.0f, 100.0f, 0.98f, dv3(0, 0, 0), 1.0f / 60.0f);
        blood_step(&b, &t.w, 1.0f / 60.0f);
    }
    for (int i = 0; i < BLOOD_SOURCES; i++) CHECK(isfinite(pending(&b, i)) && b.owed[i] >= 0.0f && b.ready[i] >= 0.0f);
    for (int i = 0; i < b.drops; i++) CHECK(isfinite(b.drop[i].pos.x) && isfinite(b.drop[i].vel.y));
    blood_free(&b);
    blood_init(&b, 0, 1u); /* a zero budget still keeps a few */
    CHECK(b.max_stains > 0);
    blood_free(&b);
    tw_free(&t);
}

static void test_blood_from_health(void)
{
    /* Each wound bleeds from where its part is, at the rate the health
     * model gives it, keeping its slot; a bandage soaks up what it can. */
    health *h = new_body(41u);
    health_cut(h, BP_LLEG, 0.6f, 1, 0.0f);
    health_cut(h, BP_RARM, 0.4f, 0, 0.0f);
    health_env e = calm_env();
    live(h, &e, 1.0);
    blood_source s[BLOOD_SOURCES];
    const dvec3 feet = dv3(5.0, 20.0, 5.0);
    const avatar_pose st = standing(0.0f);
    int n = blood_from_health(h, &st, feet, s);
    CHECK(n == 2 && h->wound_count == 2);
    CHECK(s[0].arterial == 1 && s[1].arterial == 0);
    CHECK(s[0].ml_min > 1.0f && fabsf(s[0].ml_min - h->wounds[0].bleed) < 1e-4f);
    CHECK(s[1].ml_min > 0.1f && fabsf(s[1].ml_min - h->wounds[1].bleed) < 1e-4f);
    dvec3 at = {0}, out = {0};
    blood_spot(BP_LLEG, 0, &st, feet, &at, &out);
    CHECK(fabs(s[0].at.y - at.y) < 0.15 && s[0].at.y < 21.0 && s[1].at.x > 5.1);
    /* Standing, the column of blood above the heart's level pushes harder
     * at the leg: rho g h over the ~0.85 m from heart to thigh. */
    float column = 1060.0f * 9.81f * (float)(21.3 - s[0].at.y) / 133.322f;
    CHECK(s[0].p_column > 40.0f && fabsf(s[0].p_column - column) < 1.0f);
    h->down = 30.0f; /* lying flat, no column */
    blood_from_health(h, &st, feet, s);
    CHECK(s[0].p_column == 0.0f);
    h->down = 0.0f;
    h->wounds[1].bandage = BANDAGE_STERILE;
    n = blood_from_health(h, &st, feet, s);
    CHECK(n == 2 && s[1].ml_min == 0.0f && s[0].ml_min > 1.0f);
    /* More wounds than it follows: the first BLOOD_SOURCES. */
    for (int i = 0; i < BLOOD_SOURCES + 3; i++) health_cut(h, BP_CHEST, 0.2f, 0, 0.0f);
    CHECK(blood_from_health(h, &st, feet, s) == BLOOD_SOURCES);
    mem_free(h);
}

/* Wounds bleed from the body where it is drawn: crouched, a head wound
 * comes down with the head; lying on its front, at the ground ahead of
 * the feet; and the heart's column falls with the chest. */
static void test_blood_follows_pose(void)
{
    const dvec3 feet = dv3(10.0, 20.0, 30.0);
    avatar_anim still = {0}, crouch = {.sneak = 1.0f};
    avatar_pose st = standing(0.0f), low = {0}, flat = {0};
    avatar_pose_get(&crouch, 0.0f, 0.0f, 0, &low);
    avatar_pose_get(&still, 0.0f, 0.0f, 1, &flat);
    dvec3 a0 = {0}, o0 = {0}, a1 = {0}, o1 = {0}, a2 = {0}, o2 = {0};
    blood_spot(BP_HEAD, 0, &st, feet, &a0, &o0);
    blood_spot(BP_HEAD, 0, &low, feet, &a1, &o1);
    blood_spot(BP_HEAD, 0, &flat, feet, &a2, &o2);
    double drop = (double)(st.eyes[1] - low.eyes[1]);
    CHECK(drop > 0.2 && fabs((a0.y - a1.y) - drop) < 0.08);
    CHECK(fabs(a1.y - (20.0 + (double)low.eyes[1])) < 0.15);
    CHECK(a2.y < 20.4 && a2.z < 29.0); /* yaw 0 faces -z: the head lies that way */
    CHECK(fabs(dv3_len(o1) - 1.0) < 1e-4 && fabs(dv3_len(o2) - 1.0) < 1e-4);
    /* A shin wound stays by the shin bone in a crouch. */
    blood_spot(BP_RLEG, 0, &low, feet, &a1, &o1);
    dvec3 knee = dv3(10.0 + (double)low.at[AV_HIP_R + 1][0], 20.0 + (double)low.at[AV_HIP_R + 1][1],
                     30.0 + (double)low.at[AV_HIP_R + 1][2]);
    CHECK(dv3_len(dv3(a1.x - knee.x, a1.y - knee.y, a1.z - knee.z)) < 0.25);
    health *h = new_body(42u);
    health_cut(h, BP_LLEG, 0.6f, 1, 0.0f);
    blood_source s[BLOOD_SOURCES];
    blood_from_health(h, &st, feet, s);
    float tall = s[0].p_column;
    blood_from_health(h, &low, feet, s);
    CHECK(s[0].p_column < tall - 5.0f);
    mem_free(h);
}

void test_blood_all(void)
{
    test_blood_follows_pose();
    test_blood_from_health();
    test_blood_drips();
    test_blood_fps();
    test_blood_stream();
    test_blood_pool();
    test_blood_walls();
    test_blood_merge();
    test_blood_ages();
    test_blood_water();
    test_blood_ring();
    test_blood_build();
    test_blood_spots();
    test_blood_garbage();
}
