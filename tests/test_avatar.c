/* The players' figure: it stands on its lowest point, faces where it looks,
 * no two of its boxes pass into each other in any gait, and its legs keep
 * pace with the ground the same way at any frame rate. */
#include "avatar.h"
#include "block.h"
#include "test_util.h"
#include "texgen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FACE 5 /* box order: pelvis, spine, chest, trapezius, neck, face, hair, back hair, nose */
#define NOSE 8

static const float FEET[3] = {0.0f, 0.0f, 0.0f};

static quat rot_of(const entity_instance *e) { return (quat){e->rot[0], e->rot[1], e->rot[2], e->rot[3]}; }

/* The box's corners' lowest and highest y. */
static void span_y(const entity_instance *e, float *lo, float *hi)
{
    *lo = 1e9f;
    *hi = -1e9f;
    for (int c = 0; c < 8; c++) {
        const float v[3] = {(c & 1 ? 0.5f : -0.5f) * e->scale[0], (c & 2 ? 0.5f : -0.5f) * e->scale[1],
                            (c & 4 ? 0.5f : -0.5f) * e->scale[2]};
        float r[3] = {0};
        quat_apply(rot_of(e), v, r);
        *lo = fminf(*lo, e->pos[1] + r[1]);
        *hi = fmaxf(*hi, e->pos[1] + r[1]);
    }
}

/* How far two boxes pass into each other (0 if apart), by separating
 * axes: the three faces of each and the nine edge pairs. */
static float overlap(const entity_instance *a, const entity_instance *b)
{
    float ax[3][3] = {{0}}, bx[3][3] = {{0}};
    for (int k = 0; k < 3; k++) {
        float u[3] = {0};
        u[k] = 1.0f;
        quat_apply(rot_of(a), u, ax[k]);
        quat_apply(rot_of(b), u, bx[k]);
    }
    float axes[15][3] = {{0}};
    int n = 0;
    for (int k = 0; k < 3; k++) {
        memcpy(axes[n++], ax[k], sizeof ax[k]);
        memcpy(axes[n++], bx[k], sizeof bx[k]);
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float *c = axes[n];
            c[0] = ax[i][1] * bx[j][2] - ax[i][2] * bx[j][1];
            c[1] = ax[i][2] * bx[j][0] - ax[i][0] * bx[j][2];
            c[2] = ax[i][0] * bx[j][1] - ax[i][1] * bx[j][0];
            float len = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            if (len < 1e-5f) continue; /* parallel edges: a face axis covers it */
            for (int k = 0; k < 3; k++) c[k] /= len;
            n++;
        }
    const float d[3] = {b->pos[0] - a->pos[0], b->pos[1] - a->pos[1], b->pos[2] - a->pos[2]};
    float depth = 1e9f;
    for (int i = 0; i < n; i++) {
        const float *l = axes[i];
        float ra = 0.0f, rb = 0.0f;
        for (int k = 0; k < 3; k++) {
            ra += 0.5f * a->scale[k] * fabsf(ax[k][0] * l[0] + ax[k][1] * l[1] + ax[k][2] * l[2]);
            rb += 0.5f * b->scale[k] * fabsf(bx[k][0] * l[0] + bx[k][1] * l[1] + bx[k][2] * l[2]);
        }
        float gap = fabsf(d[0] * l[0] + d[1] * l[1] + d[2] * l[2]) - ra - rb;
        if (gap >= 0.0f) return 0.0f;
        depth = fminf(depth, -gap);
    }
    return depth;
}

/* Boxes on the same bone, or on bones meeting at a joint, are meant to
 * touch there; so are the bones hanging from one joint (fingers and thumb
 * from the wrist). */
static int joined(int i, int j)
{
    int a = avatar_box_bone(i), b = avatar_box_bone(j);
    int pa = avatar_bone_parent(a), pb = avatar_bone_parent(b);
    return a == b || pa == b || pb == a || (pa == pb && pa >= AV_SHOULDER_R && pa < AV_HIP_R);
}

static float worst_overlap(const avatar_anim *a, float pitch, int *bi, int *bj)
{
    entity_instance e[AVATAR_BOXES] = {0};
    avatar_build(a, FEET, 0.0f, pitch, 0, 1.0f, e, AVATAR_BOXES);
    float worst = 0.0f;
    for (int i = 0; i < AVATAR_BOXES; i++)
        for (int j = i + 1; j < AVATAR_BOXES; j++) {
            if (joined(i, j)) continue;
            float o = overlap(&e[i], &e[j]);
            if (o > worst) {
                worst = o;
                *bi = i;
                *bj = j;
            }
        }
    return worst;
}

/* A figure that has been going at speed for a while. */
static avatar_anim moving(float speed, int sneaking, float phase)
{
    avatar_anim a = {0};
    a.speed = speed;
    a.sneak = sneaking ? 1.0f : 0.0f;
    a.phase = phase;
    a.seen = 1;
    return a;
}

static void test_avatar_boxes(void)
{
    entity_instance e[AVATAR_BOXES + 2] = {0};
    avatar_anim a = moving(0.0f, 0, 0.0f);
    CHECK(avatar_build(&a, FEET, 0.0f, 0.0f, 0, 0.8f, e, AVATAR_BOXES + 2) == AVATAR_BOXES);
    CHECK(avatar_build(&a, FEET, 0.0f, 0.0f, 0, 0.8f, e, 3) == 3);
    CHECK(avatar_build(&a, FEET, 0.0f, 0.0f, 0, 0.8f, NULL, 3) == 0);
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 0.8f, e, AVATAR_BOXES);
    /* The face is a front layer, which has 7 bits. */
    CHECK(T_FACE < 128 && (int)(e[FACE].tex >> 24 & 127u) == T_FACE);
    for (int i = 0; i < AVATAR_BOXES; i++) {
        CHECK(e[i].light == 0.8f);
        CHECK(e[i].scale[0] > 0.0f && e[i].scale[1] > 0.0f && e[i].scale[2] > 0.0f);
        CHECK(avatar_box_bone(i) >= 0 && avatar_box_bone(i) < AVATAR_BONES);
    }
    CHECK(avatar_box_bone(-1) == -1 && avatar_box_bone(AVATAR_BOXES) == -1);
    /* Parents come before children, so one pass poses the skeleton. */
    CHECK(avatar_bone_parent(AV_PELVIS) == -1 && avatar_bone_parent(AVATAR_BONES) == -1);
    for (int b = 1; b < AVATAR_BONES; b++) CHECK(avatar_bone_parent(b) >= 0 && avatar_bone_parent(b) < b);
}

static void test_avatar_stands(void)
{
    /* Standing, the crown is at the full height and the feet on the ground. */
    entity_instance e[AVATAR_BOXES] = {0};
    const float at[3] = {3.0f, 10.0f, -2.0f};
    avatar_anim a = moving(0.0f, 0, 0.0f);
    avatar_build(&a, at, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
    float low = 1e9f, high = -1e9f;
    for (int i = 0; i < AVATAR_BOXES; i++) {
        float lo = 0.0f, hi = 0.0f;
        span_y(&e[i], &lo, &hi);
        low = fminf(low, lo);
        high = fmaxf(high, hi);
    }
    CHECK(fabsf(low - 10.0f) < 1e-4f);
    CHECK(fabsf(high - 10.0f - AVATAR_HEIGHT) < 0.03f);
    CHECK(fabsf(e[0].pos[0] - 3.0f) < 0.01f && fabsf(e[0].pos[2] + 2.0f) < 0.05f); /* centred over the feet */

    /* In every gait some part touches the ground at every point of the
     * stride, and nothing goes under it. */
    const float speeds[] = {1.4f, 4.0f, 6.5f, 1.3f};
    for (int g = 0; g < 4; g++)
        for (int k = 0; k < 20; k++) {
            avatar_anim m = moving(speeds[g], g == 3, (float)k / 20.0f);
            avatar_build(&m, FEET, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
            float lo = 1e9f;
            for (int i = 0; i < AVATAR_BOXES; i++) {
                float l = 0.0f, h = 0.0f;
                span_y(&e[i], &l, &h);
                lo = fminf(lo, l);
            }
            CHECK(fabsf(lo) < 1e-4f);
        }
}

static void test_avatar_faces(void)
{
    entity_instance e[AVATAR_BOXES] = {0};
    avatar_anim a = moving(0.0f, 0, 0.0f);
    /* Yaw 0 looks toward -z; the nose is in front of the face. */
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
    CHECK(e[NOSE].pos[2] < e[FACE].pos[2] - 0.08f && fabsf(e[NOSE].pos[0]) < 1e-3f);
    CHECK(e[FACE].pos[1] > 1.5f);
    float level = e[NOSE].pos[1];
    /* A quarter turn looks toward +x, as the camera does. */
    avatar_build(&a, FEET, 0.5f * 3.14159265f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
    CHECK(e[NOSE].pos[0] > e[FACE].pos[0] + 0.08f);
    /* Looking up lifts the nose; looking down lowers it. */
    avatar_build(&a, FEET, 0.0f, 0.6f, 0, 1.0f, e, AVATAR_BOXES);
    CHECK(e[NOSE].pos[1] > level + 0.02f);
    avatar_build(&a, FEET, 0.0f, -0.6f, 0, 1.0f, e, AVATAR_BOXES);
    CHECK(e[NOSE].pos[1] < level - 0.02f);
}

static void test_avatar_no_overlap(void)
{
    /* No box passes more than 5 mm into another it isn't jointed to, in
     * any gait, at any point of the stride, looking anywhere. */
    const float speeds[] = {0.0f, 0.8f, 1.4f, 2.3f, 4.0f, 6.5f, 0.0f, 1.3f};
    float worst = 0.0f;
    int wi = -1, wj = -1, wg = -1, wk = -1, wp = 0;
    for (int g = 0; g < 8; g++)
        for (int k = 0; k < 40; k++)
            for (int p = -2; p <= 2; p++) {
                avatar_anim a = moving(speeds[g], g >= 6, (float)k / 40.0f);
                int bi = -1, bj = -1;
                float o = worst_overlap(&a, 0.75f * (float)p, &bi, &bj);
                if (o > worst) {
                    worst = o;
                    wi = bi;
                    wj = bj;
                    wg = g;
                    wk = k;
                    wp = p;
                }
            }
    if (worst > 0.005f)
        printf("  avatar: boxes %d and %d pass %.1f mm into each other (gait %d, phase %d/40, pitch %d)\n", wi, wj,
               (double)worst * 1e3, wg, wk, wp);
    CHECK(worst <= 0.005f);
}

static float phase_after(float dt, float seconds, float speed)
{
    avatar_anim a = {0};
    int steps = (int)lroundf(seconds / dt);
    for (int i = 0; i <= steps; i++) avatar_anim_update(&a, (double)(speed * dt * (float)i), 0.0, 0, 0, dt);
    return a.phase;
}

static void test_avatar_pace(void)
{
    /* The stride lengthens with speed and the cadence stays human: about
     * 112 steps a minute at a 1.4 m/s walk, over 200 sprinting. */
    float last = 0.0f;
    for (int i = 0; i <= 32; i++) {
        float s = avatar_stride(0.25f * (float)i, 0.0f);
        CHECK(s >= last - 1e-6f && s > 0.5f && s <= 3.4f);
        last = s;
    }
    float walk = 2.0f * 1.4f / avatar_stride(1.4f, 0.0f) * 60.0f;
    float sprint = 2.0f * 6.5f / avatar_stride(6.5f, 0.0f) * 60.0f;
    CHECK(walk > 90.0f && walk < 125.0f);
    CHECK(sprint > 160.0f && sprint < 240.0f);
    CHECK(avatar_stride(1.3f, 1.0f) < avatar_stride(1.3f, 0.0f)); /* sneaking takes short steps */
    CHECK(avatar_stride(NAN, 0.0f) > 0.5f && avatar_stride(-3.0f, 0.0f) > 0.5f);

    /* The phase follows the distance, so it is the same at 30 and 144 fps. */
    float p30 = phase_after(1.0f / 30.0f, 3.0f, 4.0f), p144 = phase_after(1.0f / 144.0f, 3.0f, 4.0f);
    float d = fabsf(p30 - p144);
    CHECK(fminf(d, 1.0f - d) < 0.02f);

    /* Flying, standing still and teleporting don't move the legs. */
    avatar_anim a = {0};
    avatar_anim_update(&a, 0.0, 0.0, 0, 0, 1.0f / 60.0f);
    avatar_anim_update(&a, 0.1, 0.0, 0, 1, 1.0f / 60.0f);
    CHECK(a.phase == 0.0f && a.speed == 0.0f);
    avatar_anim_update(&a, 500.0, 0.0, 0, 0, 1.0f / 60.0f);
    CHECK(a.phase == 0.0f && a.speed == 0.0f && a.x == 500.0);
}

static void test_avatar_poses(void)
{
    entity_instance e[AVATAR_BOXES] = {0};
    avatar_anim a = moving(0.0f, 0, 0.0f);
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
    float stand = e[FACE].pos[1];
    /* Sneaking crouches about a quarter lower. */
    a.sneak = 1.0f;
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
    CHECK(e[FACE].pos[1] < stand - 0.2f && e[FACE].pos[1] > 0.9f);
    float crouched = e[FACE].pos[1];
    /* Half eased into the sneak, the head is between the two heights. */
    a.sneak = 0.5f;
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
    CHECK(e[FACE].pos[1] < stand - 0.02f);
    CHECK(e[FACE].pos[1] > crouched + 0.02f);
    a.sneak = 1.0f;
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
    CHECK(fabsf(e[FACE].pos[1] - crouched) < 1e-5f);
    /* The sneak eases in rather than snapping. */
    avatar_anim b = {0};
    avatar_anim_update(&b, 0.0, 0.0, 1, 0, 1.0f / 60.0f);
    CHECK(b.sneak > 0.0f && b.sneak < 0.2f);
    /* Dead, it lies flat, face down along its facing. */
    a.sneak = 0.0f;
    avatar_build(&a, FEET, 0.0f, 0.0f, 1, 1.0f, e, AVATAR_BOXES);
    for (int i = 0; i < AVATAR_BOXES; i++) {
        float lo = 0.0f, hi = 0.0f;
        span_y(&e[i], &lo, &hi);
        CHECK(hi < 0.4f && lo > -1e-4f);
    }
    CHECK(e[FACE].pos[2] < -1.2f);
    CHECK(e[NOSE].pos[1] < e[FACE].pos[1]);
}

static void test_avatar_garbage(void)
{
    /* A broken state or a NaN position from the network draws a standing
     * figure rather than NaN boxes. */
    avatar_anim a = {0};
    a.phase = NAN;
    a.speed = INFINITY;
    avatar_anim_update(&a, (double)NAN, 0.0, 0, 0, 1.0f / 60.0f);
    CHECK(isfinite(a.phase) && isfinite(a.speed));
    avatar_anim_update(&a, 1.0, 1.0, 0, 0, NAN);
    avatar_anim_update(&a, 1.1, 1.0, 0, 0, -1.0f);
    CHECK(isfinite(a.phase) && isfinite(a.speed) && a.phase >= 0.0f && a.phase < 1.0f);
    avatar_anim bad = {NAN, NAN, NAN, NAN, 0.0, 0.0, 1};
    entity_instance e[AVATAR_BOXES] = {0};
    CHECK(avatar_build(&bad, FEET, NAN, INFINITY, 0, 1.0f, e, AVATAR_BOXES) == AVATAR_BOXES);
    for (int i = 0; i < AVATAR_BOXES; i++)
        for (int k = 0; k < 4; k++) CHECK(isfinite(e[i].pos[k % 3]) && isfinite(e[i].rot[k]));
}

/* The box hanging from a bone on the given side (0 right, 1 left): the
 * first one whose bone is that side's first bone plus k. */
static int side_box(int first_r, int first_l, int side, int k)
{
    int bone = (side ? first_l : first_r) + k;
    for (int i = 0; i < AVATAR_BOXES; i++)
        if (avatar_box_bone(i) == bone) return i;
    CHECK(0); /* no box on that bone */
    return 0;
}

static void test_avatar_symmetry(void)
{
    /* Standing still with the breath at rest, the left side mirrors the
     * right across x = 0, and the trunk and head sit on the midline. */
    entity_instance e[AVATAR_BOXES] = {0};
    avatar_anim a = moving(0.0f, 0, 0.0f);
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
    for (int k = 0; k < 5; k++) {
        const entity_instance *r = &e[side_box(AV_SHOULDER_R, AV_SHOULDER_L, 0, k)];
        const entity_instance *l = &e[side_box(AV_SHOULDER_R, AV_SHOULDER_L, 1, k)];
        CHECK(fabsf(r->pos[0] + l->pos[0]) < 1e-4f && fabsf(r->pos[1] - l->pos[1]) < 1e-4f);
        CHECK(fabsf(r->pos[2] - l->pos[2]) < 1e-4f && r->pos[0] > 0.0f);
    }
    for (int k = 0; k < 3; k++) {
        const entity_instance *r = &e[side_box(AV_HIP_R, AV_HIP_L, 0, k)];
        const entity_instance *l = &e[side_box(AV_HIP_R, AV_HIP_L, 1, k)];
        CHECK(fabsf(r->pos[0] + l->pos[0]) < 1e-4f && fabsf(r->pos[1] - l->pos[1]) < 1e-4f);
        CHECK(r->pos[0] > 0.0f);
    }
    for (int i = 0; i <= NOSE; i++) CHECK(fabsf(e[i].pos[0]) < 1e-4f);
    /* Every box hangs from a bone that leads back to the pelvis. */
    for (int i = 0; i < AVATAR_BOXES; i++) {
        int b = avatar_box_bone(i), steps = 0;
        while (b != AV_PELVIS && b >= 0 && steps++ < AVATAR_BONES) b = avatar_bone_parent(b);
        CHECK(b == AV_PELVIS);
    }
}

static void test_avatar_stride_shape(void)
{
    /* Walking, the right foot leads at heel strike and the left half a
     * stride later, and each arm swings forward with the other leg. */
    const int foot_r = side_box(AV_HIP_R, AV_HIP_L, 0, 2), foot_l = side_box(AV_HIP_R, AV_HIP_L, 1, 2);
    const int hand_r = side_box(AV_SHOULDER_R, AV_SHOULDER_L, 0, 2);
    const int hand_l = side_box(AV_SHOULDER_R, AV_SHOULDER_L, 1, 2);
    entity_instance e[AVATAR_BOXES] = {0};
    avatar_anim a = moving(1.4f, 0, 0.0f);
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
    CHECK(e[foot_r].pos[2] < e[foot_l].pos[2] - 0.3f); /* forward is -z */
    CHECK(e[hand_l].pos[2] < e[hand_r].pos[2] - 0.05f);
    a.phase = 0.5f;
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
    CHECK(e[foot_l].pos[2] < e[foot_r].pos[2] - 0.3f);
    CHECK(e[hand_r].pos[2] < e[hand_l].pos[2] - 0.05f);

    /* Running leans further forward than walking: the head is further
     * ahead of the pelvis. */
    float ahead[2] = {0};
    const float speeds[2] = {1.4f, 6.5f};
    for (int g = 0; g < 2; g++) {
        float sum = 0.0f;
        for (int k = 0; k < 8; k++) {
            avatar_anim m = moving(speeds[g], 0, (float)k / 8.0f);
            avatar_build(&m, FEET, 0.0f, 0.0f, 0, 1.0f, e, AVATAR_BOXES);
            sum += e[0].pos[2] - e[FACE].pos[2];
        }
        ahead[g] = sum / 8.0f;
    }
    CHECK(ahead[1] > ahead[0] + 0.05f);
}

static void test_avatar_turns(void)
{
    /* Turning the yaw turns the whole figure about its feet: every box
     * keeps its height and its distance from the vertical axis. */
    entity_instance a0[AVATAR_BOXES] = {0}, a1[AVATAR_BOXES] = {0};
    avatar_anim a = moving(4.0f, 0, 0.3f);
    avatar_build(&a, FEET, 0.0f, 0.2f, 0, 1.0f, a0, AVATAR_BOXES);
    avatar_build(&a, FEET, 2.1f, 0.2f, 0, 1.0f, a1, AVATAR_BOXES);
    float c = cosf(2.1f), s = sinf(2.1f);
    for (int i = 0; i < AVATAR_BOXES; i++) {
        CHECK(fabsf(a0[i].pos[1] - a1[i].pos[1]) < 1e-4f);
        /* Turning by yaw toward +x is -yaw about +y. */
        float x = c * a0[i].pos[0] - s * a0[i].pos[2], z = s * a0[i].pos[0] + c * a0[i].pos[2];
        CHECK(fabsf(x - a1[i].pos[0]) < 1e-4f && fabsf(z - a1[i].pos[2]) < 1e-4f);
    }
}

static void test_avatar_breath(void)
{
    /* Breathing moves only the chest, and never by more than a few
     * millimetres. */
    entity_instance a0[AVATAR_BOXES] = {0}, a1[AVATAR_BOXES] = {0};
    avatar_anim a = moving(0.0f, 0, 0.0f);
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 1.0f, a0, AVATAR_BOXES);
    a.time = 1.0f; /* a quarter breath in: full inhale */
    avatar_build(&a, FEET, 0.0f, 0.0f, 0, 1.0f, a1, AVATAR_BOXES);
    CHECK(a1[2].scale[2] > a0[2].scale[2] + 0.002f && a1[2].scale[2] < a0[2].scale[2] + 0.01f);
    CHECK(fabsf(a1[0].pos[1] - a0[0].pos[1]) < 1e-4f); /* the hips stay put */
    for (int i = 0; i < AVATAR_BOXES; i++) {
        float d = fabsf(a1[i].pos[0] - a0[i].pos[0]) + fabsf(a1[i].pos[1] - a0[i].pos[1]) +
                  fabsf(a1[i].pos[2] - a0[i].pos[2]);
        CHECK(d < 0.01f);
    }
    /* The clock wraps instead of losing precision over a long session. */
    avatar_anim b = {0};
    for (int i = 0; i < 200; i++) avatar_anim_update(&b, 0.0, 0.0, 0, 0, 30.0f);
    CHECK(b.time >= 0.0f && b.time < 3600.0f);
}

static void test_avatar_settles(void)
{
    /* The speed follows a change of pace within a fraction of a second,
     * and sneaking eases out again when it stops. */
    avatar_anim a = {0};
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i <= 60; i++) avatar_anim_update(&a, (double)(2.0f * dt * (float)i), 0.0, 1, 0, dt);
    CHECK(fabsf(a.speed - 2.0f) < 0.05f && a.sneak > 0.99f);
    for (int i = 61; i <= 120; i++) avatar_anim_update(&a, (double)(2.0f * dt * 60.0f), 0.0, 0, 0, dt);
    CHECK(a.speed < 0.05f && a.sneak < 0.01f);
    float p = a.phase;
    avatar_anim_update(&a, (double)(2.0f * dt * 60.0f), 0.0, 0, 0, dt);
    CHECK(a.phase == p); /* standing still, the stride waits */
}

static void test_avatar_textures(void)
{
    /* The linen, the shorts, the hair and the skin are told apart at a
     * glance, and the face has dark pupils on light eyes. */
    const int layers[4] = {T_LINEN, T_LINEN_DARK, T_HAIR, T_SKIN};
    int mean[4] = {0};
    for (int l = 0; l < 4; l++) {
        int sum = 0;
        for (int y = 0; y < TEX_SIZE; y++)
            for (int x = 0; x < TEX_SIZE; x++) {
                uint8_t px[4] = {0};
                texgen_texel(layers[l], x, y, px);
                CHECK(px[3] == 255);
                sum += px[0] + px[1] + px[2];
            }
        mean[l] = sum / (TEX_SIZE * TEX_SIZE);
    }
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++) CHECK(abs(mean[i] - mean[j]) > 20);
    uint8_t pupil[4] = {0}, white[4] = {0}, cheek[4] = {0};
    texgen_texel(T_FACE, 5, 7, pupil);
    texgen_texel(T_FACE, 4, 7, white);
    texgen_texel(T_FACE, 2, 9, cheek);
    CHECK(pupil[0] < 80 && white[0] > 200 && cheek[0] > pupil[0] + 100);
}

void test_avatar_all(void)
{
    test_avatar_symmetry();
    test_avatar_stride_shape();
    test_avatar_turns();
    test_avatar_breath();
    test_avatar_settles();
    test_avatar_textures();
    test_avatar_boxes();
    test_avatar_stands();
    test_avatar_faces();
    test_avatar_no_overlap();
    test_avatar_pace();
    test_avatar_poses();
    test_avatar_garbage();
}
