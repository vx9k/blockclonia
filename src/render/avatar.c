#include "avatar.h"
#include "anim.h"
#include "block.h"

#include <math.h>
#include <string.h>

/* Frame: x is the figure's right, y up, -z forward, metres, origin under
 * the feet. A bone turns about its joint by XYZ Euler angles (the matrix
 * is Rx Ry Rz); positive x turns a hanging limb forward, so hip, shoulder
 * and elbow flexion are positive and knee flexion negative. */

#define PI_F 3.14159265f
#define DEG (PI_F / 180.0f)

/* Joint heights and segment lengths as fractions of the height (Drillis &
 * Contini 1966, as tabulated in Winter, Biomechanics and Motor Control of
 * Human Movement). */
#define HIP_Y (0.530f * AVATAR_HEIGHT)
#define KNEE_Y (0.285f * AVATAR_HEIGHT)
#define ANKLE_Y (0.039f * AVATAR_HEIGHT)
#define SHOULDER_Y (0.818f * AVATAR_HEIGHT)
#define CHIN_Y (0.870f * AVATAR_HEIGHT)
#define UPPER_ARM (0.186f * AVATAR_HEIGHT)
#define FOREARM (0.146f * AVATAR_HEIGHT)
#define FOOT_LEN (0.152f * AVATAR_HEIGHT)
#define SHOULDER_W (0.259f * AVATAR_HEIGHT) /* between the shoulder joints' outer edges */
#define HIP_W (0.191f * AVATAR_HEIGHT)
#define HEAD_H (AVATAR_HEIGHT - CHIN_Y)
#define THIGH (HIP_Y - KNEE_Y)
#define SHANK (KNEE_Y - ANKLE_Y)
#define CHEST_AT 0.29f                           /* chest joint above the hips */
#define CHEST_TOP (SHOULDER_Y - HIP_Y - CHEST_AT) /* shoulder joints in the chest's frame */
#define HEEL 0.065f                              /* the ankle sits this far forward of the heel */

static const struct {
    int parent;
    float at[3];
} BONES[AVATAR_BONES] = {
    [AV_PELVIS] = {-1, {0.0f, HIP_Y, 0.0f}},
    [AV_SPINE] = {AV_PELVIS, {0.0f, 0.12f, 0.0f}},
    [AV_CHEST] = {AV_SPINE, {0.0f, CHEST_AT - 0.12f, 0.0f}},
    [AV_NECK] = {AV_CHEST, {0.0f, CHEST_TOP + 0.04f, 0.0f}},
    [AV_HEAD] = {AV_NECK, {0.0f, CHIN_Y - SHOULDER_Y - 0.04f, 0.0f}},
#define ARM(s, sx)                                                                          \
    [AV_SHOULDER_##s] = {AV_CHEST, {(sx) * (SHOULDER_W * 0.5f - 0.025f), CHEST_TOP, 0.0f}}, \
    [AV_SHOULDER_##s + 1] = {AV_SHOULDER_##s, {0.0f, -UPPER_ARM, 0.0f}},                    \
    [AV_SHOULDER_##s + 2] = {AV_SHOULDER_##s + 1, {0.0f, -FOREARM, 0.0f}},                  \
    [AV_SHOULDER_##s + 3] = {AV_SHOULDER_##s + 2, {0.0f, -0.1f, 0.004f}},                   \
    [AV_SHOULDER_##s + 4] = {AV_SHOULDER_##s + 2, {(sx) * -0.012f, -0.025f, -0.04f}}
    ARM(R, 1.0f),
    ARM(L, -1.0f),
#undef ARM
/* The hip joints sit inside the pelvis box, so the thighs clear the
     * hands hanging beside them. */
#define LEG(s, sx)                                                           \
    [AV_HIP_##s] = {AV_PELVIS, {(sx) * (HIP_W * 0.5f - 0.02f), 0.0f, 0.0f}}, \
    [AV_HIP_##s + 1] = {AV_HIP_##s, {0.0f, -THIGH, 0.0f}}, [AV_HIP_##s + 2] = {AV_HIP_##s + 1, {0.0f, -SHANK, 0.0f}}
    LEG(R, 1.0f),
    LEG(L, -1.0f),
#undef LEG
};

/* The boxes: a bone, width (x) and depth (z), the span y0..y1 along the
 * bone, the centre's x and z, and the layers. The chest is 0.30 m across
 * at the armpits and the shoulder joints sit outside it, so the upper arms
 * clear the ribs. */
/* Laid out by hand: clang-format versions disagree on the braces inside ARM and LEG. */
/* clang-format off */
#define SKIN ((uint32_t)T_SKIN * 0x010101u)
#define LINEN ((uint32_t)T_LINEN * 0x010101u)
#define SHORTS ((uint32_t)T_LINEN_DARK * 0x010101u)
#define HAIR ((uint32_t)T_HAIR * 0x010101u)
static const struct {
    int bone;
    float w, d, y0, y1, x, z;
    uint32_t tex;
} BOXES[AVATAR_BOXES] = {
    {AV_PELVIS, 0.31f, 0.2f, -0.06f, 0.13f, 0.0f, 0.0f, SHORTS},
    {AV_SPINE, 0.28f, 0.18f, 0.0f, 0.17f, 0.0f, 0.0f, LINEN},
    {AV_CHEST, 0.30f, 0.21f, 0.0f, CHEST_TOP + 0.02f, 0.0f, 0.0f, LINEN},
    {AV_CHEST, 0.22f, 0.12f, CHEST_TOP + 0.02f, CHEST_TOP + 0.05f, 0.0f, 0.0f, SKIN}, /* trapezius */
    {AV_NECK, 0.11f, 0.11f, -0.02f, CHIN_Y - SHOULDER_Y - 0.02f, 0.0f, 0.0f, SKIN},
    {AV_HEAD, 0.16f, 0.2f, 0.0f, HEAD_H - 0.025f, 0.0f, -0.01f, 0},                   /* face: tex set below */
    {AV_HEAD, 0.168f, 0.21f, HEAD_H - 0.05f, HEAD_H, 0.0f, 0.0f, HAIR},
    {AV_HEAD, 0.168f, 0.06f, 0.07f, HEAD_H - 0.03f, 0.0f, 0.08f, HAIR},
    {AV_HEAD, 0.03f, 0.03f, 0.06f, 0.1f, 0.0f, -0.12f, SKIN}, /* nose */
#define ARM(s)                                                                          \
    {AV_SHOULDER_##s, 0.095f, 0.1f, 0.03f, -UPPER_ARM, 0.0f, 0.0f, SKIN},               \
        {AV_SHOULDER_##s + 1, 0.08f, 0.075f, 0.0f, -0.16f, 0.0f, 0.0f, SKIN},           \
        {AV_SHOULDER_##s + 1, 0.05f, 0.062f, -0.155f, -FOREARM + 0.01f, 0.0f, 0.0f, SKIN}, /* tapers to the wrist */ \
        {AV_SHOULDER_##s + 2, 0.032f, 0.085f, 0.0f, -0.1f, 0.0f, 0.0f, SKIN},           \
        {AV_SHOULDER_##s + 3, 0.026f, 0.078f, 0.0f, -0.085f, 0.0f, 0.0f, SKIN},         \
        {AV_SHOULDER_##s + 4, 0.024f, 0.024f, 0.0f, -0.06f, 0.0f, 0.0f, SKIN}
    ARM(R),
    ARM(L),
#undef ARM
#define LEG(s)                                                                 \
    {AV_HIP_##s, 0.135f, 0.15f, 0.02f, -0.16f, 0.0f, 0.0f, SHORTS},            \
        {AV_HIP_##s, 0.13f, 0.14f, -0.16f, -THIGH, 0.0f, 0.0f, SKIN},          \
        {AV_HIP_##s + 1, 0.1f, 0.11f, 0.0f, -SHANK + 0.01f, 0.0f, 0.0f, SKIN}, \
        {AV_HIP_##s + 2, 0.09f, FOOT_LEN, 0.015f, -ANKLE_Y, 0.0f, -(FOOT_LEN * 0.5f - HEEL), SKIN}
    LEG(R),
    LEG(L),
#undef LEG
    /* clang-format on */
};
#define FACE_BOX 5
#define CHEST_BOX 2

int avatar_box_bone(int box) { return box >= 0 && box < AVATAR_BOXES ? BOXES[box].bone : -1; }

int avatar_bone_parent(int bone) { return bone >= 0 && bone < AVATAR_BONES ? BONES[bone].parent : -1; }

/* ---- gaits */

/* A joint's angle through the stride: degrees at percentages of it. */
typedef struct {
    float at, deg;
} key;

/* Leg angles from gait-lab recordings of adults (Perry's phases: heel
 * strike at 0%, toe off near 60% walking and 40% running), smoothed. The
 * arm swings against the opposite leg, its shoulder angle scaled from that
 * leg's hip between hip_lo and hip_hi; the elbow bends more as it comes
 * forward. lean is the trunk's forward tilt, twist the pelvis's turn and
 * drop its sideways dip on the swing side, all in degrees. */
typedef struct {
    key hip[10], knee[10], ankle[10];
    int nhip, nknee, nankle;
    float hip_lo, hip_hi, shoulder, elbow_back, elbow_fwd, lean, twist, drop, abduct;
} gait;

static const gait WALK = {
    {{0, 30}, {12, 26}, {30, 10}, {50, -8}, {57, -10}, {65, -2}, {78, 22}, {88, 32}, {100, 30}},
    {{0, 3}, {15, 18}, {32, 8}, {42, 5}, {52, 12}, {62, 38}, {72, 62}, {84, 36}, {95, 4}, {100, 3}},
    {{0, 0}, {8, -6}, {28, 4}, {48, 10}, {62, -18}, {72, -8}, {86, 0}, {100, 0}},
    9,
    10,
    8,
    -10.0f,
    32.0f,
    15.0f,
    12.0f,
    30.0f,
    3.0f,
    4.0f,
    3.0f,
    8.0f,
};
static const gait RUN = {
    {{0, 48}, {14, 26}, {30, 0}, {44, -18}, {52, -14}, {66, 30}, {78, 66}, {90, 58}, {100, 48}},
    {{0, 22}, {12, 42}, {30, 26}, {44, 18}, {56, 60}, {68, 108}, {80, 96}, {92, 40}, {100, 22}},
    {{0, 4}, {12, 16}, {30, 8}, {44, -26}, {58, -18}, {76, 4}, {100, 4}},
    9,
    9,
    7,
    -18.0f,
    66.0f,
    42.0f,
    85.0f,
    100.0f,
    13.0f,
    8.0f,
    2.0f,
    12.0f,
};
static const gait SNEAK = {
    {{0, 52}, {20, 44}, {45, 26}, {55, 24}, {70, 44}, {85, 60}, {100, 52}},
    {{0, 48}, {20, 54}, {45, 42}, {60, 60}, {75, 82}, {90, 58}, {100, 48}},
    {{0, 8}, {25, 18}, {48, 20}, {62, -4}, {80, 4}, {100, 8}},
    7,
    7,
    6,
    24.0f,
    60.0f,
    8.0f,
    40.0f,
    55.0f,
    24.0f,
    3.0f,
    2.0f,
    8.0f,
};

/* A monotone cubic through periodic keyframes (Fritsch-Carlson with the
 * harmonic mean of the neighbouring slopes), so a joint never overshoots
 * past a recorded angle the way a plain spline would. x is in percent. */
static float sample(const key *k, int n, float x)
{
    x = fmodf(x, 100.0f);
    if (x < 0.0f) x += 100.0f;
    int i = 0;
    while (i < n - 2 && x > k[i + 1].at) i++;
    float d[3] = {0}; /* slopes before, of and after the piece i..i+1 */
    for (int j = 0; j < 3; j++) {
        int a = i - 1 + j;
        if (a < 0) a = n - 2; /* the last piece wraps round to the first */
        if (a > n - 2) a = 0;
        d[j] = (k[a + 1].deg - k[a].deg) / (k[a + 1].at - k[a].at);
    }
    float m0 = d[0] * d[1] <= 0.0f ? 0.0f : 2.0f * d[0] * d[1] / (d[0] + d[1]);
    float m1 = d[1] * d[2] <= 0.0f ? 0.0f : 2.0f * d[1] * d[2] / (d[1] + d[2]);
    float h = k[i + 1].at - k[i].at, t = (x - k[i].at) / h, t2 = t * t, t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * k[i].deg + (t3 - 2.0f * t2 + t) * h * m0 +
           (-2.0f * t3 + 3.0f * t2) * k[i + 1].deg + (t3 - t2) * h * m1;
}

/* Joint angles in degrees, per side (0 right, 1 left), in the gait tables'
 * sense: flexion positive, ankle positive toes up. */
typedef struct {
    float hip[2], knee[2], ankle[2], shoulder[2], elbow[2];
    float stance, abduct, lean, twist, drop, chest_twist;
} joints;

static void gait_joints(const gait *g, float phase, joints *j)
{
    float ph = phase * 100.0f;
    float mid = 0.5f * (g->hip_hi + g->hip_lo), half = 0.5f * (g->hip_hi - g->hip_lo);
    for (int s = 0; s < 2; s++) {
        float p = s ? ph + 50.0f : ph;
        j->hip[s] = sample(g->hip, g->nhip, p);
        j->knee[s] = sample(g->knee, g->nknee, p);
        j->ankle[s] = sample(g->ankle, g->nankle, p);
        /* The arm follows the other leg's hip about 5% of a stride late. */
        float fwd = (sample(g->hip, g->nhip, p + 50.0f - 5.0f) - mid) / half;
        j->shoulder[s] = g->shoulder * fwd;
        j->elbow[s] = g->elbow_back + (g->elbow_fwd - g->elbow_back) * fmaxf(0.0f, fwd);
    }
    float a = 2.0f * PI_F * phase;
    j->stance = 0.0f;
    j->abduct = g->abduct;
    j->lean = g->lean;
    j->twist = g->twist * cosf(a);      /* the pelvis turns with the leading leg */
    j->drop = g->drop * sinf(2.0f * a); /* and dips on the swing side, once a step */
    j->chest_twist = -1.2f * j->twist;  /* the chest turns against it */
}

/* Standing: knees just off locked, feet a little apart, arms hanging clear
 * of the thighs. Crouched (sneaking in place): hips and knees bent with
 * the feet flat (ankle = knee - hip) and the trunk over the feet. */
static const joints STAND = {{0, 0}, {2, 2}, {0, 0}, {0, 0}, {10, 10}, 2.0f, 8.0f, 0, 0, 0, 0};
static const joints CROUCH = {{60, 60}, {75, 75}, {15, 15}, {5, 5}, {40, 40}, 3.0f, 8.0f, 24.0f, 0, 0, 0};

static void mix(joints *a, const joints *b, float t)
{
    float *x = (float *)a;
    const float *y = (const float *)b;
    for (size_t i = 0; i < sizeof(joints) / sizeof(float); i++) x[i] += (y[i] - x[i]) * t;
}

static float smooth(float lo, float hi, float v)
{
    float t = anim_clamp01((v - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

/* Below about 2 m/s people walk; by 2.8 m/s they run (the walk-run
 * transition is near 2 m/s). The game's 4 m/s walk is a jog. */
static float run_weight(float speed) { return smooth(1.8f, 2.8f, speed); }

float avatar_stride(float speed, float sneak)
{
    /* Stride grows with speed until the legs run out of reach, then the
     * cadence rises instead: 1.5 m at a 1.4 m/s walk (about 112 steps a
     * minute), 2.5 m jogging at 4 m/s, 3.4 m sprinting. */
    float v = isfinite(speed) ? fmaxf(speed, 0.0f) : 0.0f;
    float walk = fminf(0.65f + 0.6f * v, 1.6f), run = fminf(1.2f + 0.33f * v, 3.4f);
    float r = run_weight(v), s = isfinite(sneak) ? anim_clamp01(sneak) : 0.0f;
    return (1.0f - s) * (walk + (run - walk) * r) + s * 0.9f;
}

/* ---- animation state */

#define SPEED_TAU 0.15f /* s: the gait follows a change of pace this quickly */
#define SNEAK_RATE 5.0f /* 1/s */
#define TELEPORT 4.0    /* m in one frame */
#define BREATH_S 4.0f   /* 15 breaths a minute */
#define TIME_WRAP 3600.0f

void avatar_anim_update(avatar_anim *a, double x, double z, int sneaking, int flying, float dt)
{
    if (!isfinite(a->phase) || !isfinite(a->speed) || !isfinite(a->sneak) || !isfinite(a->time))
        memset(a, 0, sizeof *a);
    if (!isfinite(dt) || dt < 0.0f) dt = 0.0f;
    if (!isfinite(x) || !isfinite(z)) return;
    double dx = a->seen ? x - a->x : 0.0, dz = a->seen ? z - a->z : 0.0;
    double dist = sqrt(dx * dx + dz * dz);
    if (dist > TELEPORT) {
        dist = 0.0;
        a->speed = 0.0f;
    }
    a->x = x;
    a->z = z;
    a->seen = 1;
    a->time = fmodf(a->time + dt, TIME_WRAP);
    a->sneak = anim_approach(a->sneak, sneaking ? 1.0f : 0.0f, SNEAK_RATE, dt);
    if (flying) dist = 0.0;
    float before = avatar_stride(a->speed, a->sneak);
    if (dt > 0.0f) {
        float v = (float)(dist / (double)dt);
        a->speed += (v - a->speed) * (1.0f - expf(-dt / SPEED_TAU));
    }
    /* The stride moves on with the ground covered, not the clock, so the
     * feet keep pace at any frame rate; averaging the stride over the
     * frame keeps a change of pace from drifting with the frame rate. */
    float per_m = 0.5f * (1.0f / before + 1.0f / avatar_stride(a->speed, a->sneak));
    a->phase = fmodf(a->phase + (float)dist * per_m, 1.0f);
}

/* ---- posing */

typedef struct {
    quat q;
    float p[3];
} xform;

static quat euler(float x, float y, float z)
{
    return quat_mul(quat_axis(1.0f, 0.0f, 0.0f, x),
                    quat_mul(quat_axis(0.0f, 1.0f, 0.0f, y), quat_axis(0.0f, 0.0f, 1.0f, z)));
}

/* Local rotations of every bone for these joint angles. */
static void bone_turns(const joints *j, float pitch, float breath, quat *r)
{
    for (int b = 0; b < AVATAR_BONES; b++) r[b] = quat_identity();
    r[AV_PELVIS] = euler(0.0f, j->twist * DEG, -j->drop * DEG);
    r[AV_SPINE] = euler(-j->lean * DEG, 0.0f, 0.0f);
    r[AV_CHEST] = euler(0.6f * DEG * breath, j->chest_twist * DEG, 0.0f);
    /* The head tilts back against the trunk's lean so the eyes stay level,
     * and the neck and head share the look up or down, the neck taking
     * more (bending at its base carries the head clear of the chest). The
     * head bows about 37 degrees and tips back about 43 from the trunk,
     * counting what it already tips over a lean; the eyes do the rest.
     * Further, a box chin or skull would sink into the chest or shoulders
     * where a real one only meets them. */
    float up = 0.75f - 0.8f * j->lean * DEG;
    float look = fminf(fmaxf(pitch, -0.65f), up);
    r[AV_NECK] = euler(0.8f * j->lean * DEG + 0.6f * look, 0.0f, 0.0f);
    r[AV_HEAD] = euler(0.4f * look, 0.0f, 0.0f);
    for (int s = 0; s < 2; s++) {
        float sx = s ? -1.0f : 1.0f;
        int arm = s ? AV_SHOULDER_L : AV_SHOULDER_R, leg = s ? AV_HIP_L : AV_HIP_R;
        r[arm] = euler(j->shoulder[s] * DEG, 0.0f, sx * j->abduct * DEG);
        r[arm + 1] = euler(j->elbow[s] * DEG, 0.0f, 0.0f);
        r[arm + 3] = euler(0.0f, 0.0f, -sx * 25.0f * DEG); /* fingers curled toward the thigh */
        r[arm + 4] = euler(-0.35f, 0.0f, 0.0f);
        r[leg] = euler(j->hip[s] * DEG, 0.0f, sx * j->stance * DEG);
        r[leg + 1] = euler(-j->knee[s] * DEG, 0.0f, 0.0f);
        r[leg + 2] = euler(j->ankle[s] * DEG, 0.0f, 0.0f);
    }
}

static void pose_joints(const avatar_anim *a, joints *j)
{
    float v = isfinite(a->speed) ? fmaxf(a->speed, 0.0f) : 0.0f, s = isfinite(a->sneak) ? anim_clamp01(a->sneak) : 0.0f;
    float phase = isfinite(a->phase) ? a->phase : 0.0f;
    joints walk = {0}, run = {0}, sneak = {0};
    gait_joints(&WALK, phase, &walk);
    gait_joints(&RUN, phase, &run);
    gait_joints(&SNEAK, phase, &sneak);
    mix(&walk, &run, run_weight(v));
    mix(&walk, &sneak, s);
    *j = STAND;
    mix(j, &CROUCH, s);
    mix(j, &walk, smooth(0.05f, 0.6f, v)); /* from standing into the stride as it gets going */
}

/* The eyes, in the head bone's frame: halfway up the head, at the face. */
static const float EYES[3] = {0.0f, 0.115f, -0.1f};

/* Poses every box with the root at the origin, before standing the figure
 * on its lowest point (returned). eyes gets where the eyes are, and bones
 * (if not NULL) every bone. */
static float pose(const avatar_anim *a, float yaw, float pitch, int dead, float light, entity_instance *e,
                  float eyes[3], xform *bones)
{
    joints j = {0};
    pose_joints(a, &j);
    float t = isfinite(a->time) ? a->time : 0.0f;
    float breath = sinf(2.0f * PI_F * t / BREATH_S);
    quat turn[AVATAR_BONES] = {{0}};
    bone_turns(&j, isfinite(pitch) ? pitch : 0.0f, breath, turn);

    /* Facing yaw: forward is -z turned toward +x, so -yaw about +y. Dead,
     * the figure falls forward onto its front. */
    quat root = quat_axis(0.0f, 1.0f, 0.0f, isfinite(yaw) ? -yaw : 0.0f);
    if (dead) root = quat_mul(root, quat_axis(1.0f, 0.0f, 0.0f, -0.5f * PI_F));
    xform w[AVATAR_BONES] = {0};
    for (int b = 0; b < AVATAR_BONES; b++) {
        int p = BONES[b].parent;
        quat pq = p < 0 ? root : w[p].q;
        float off[3] = {0};
        quat_apply(pq, BONES[b].at, off);
        for (int k = 0; k < 3; k++) w[b].p[k] = (p < 0 ? 0.0f : w[p].p[k]) + off[k];
        w[b].q = quat_norm(quat_mul(pq, turn[b]));
    }
    if (bones) memcpy(bones, w, sizeof w);

    float at[3] = {0};
    quat_apply(w[AV_HEAD].q, EYES, at);
    for (int k = 0; k < 3; k++) eyes[k] = w[AV_HEAD].p[k] + at[k];
    float low = 1e9f;
    for (int i = 0; i < AVATAR_BOXES; i++) {
        const xform *x = &w[BOXES[i].bone];
        float sy = 1.0f, sz = 1.0f;
        if (i == CHEST_BOX) { /* the ribs rise and the chest deepens a little with each breath */
            sy += 0.008f * breath;
            sz += 0.02f * breath;
        }
        const float c[3] = {BOXES[i].x, 0.5f * (BOXES[i].y0 + BOXES[i].y1), BOXES[i].z};
        float r[3] = {0};
        quat_apply(x->q, c, r);
        for (int k = 0; k < 3; k++) e[i].pos[k] = x->p[k] + r[k];
        entity_set_rot(&e[i], x->q);
        e[i].scale[0] = BOXES[i].w;
        e[i].scale[1] = fabsf(BOXES[i].y1 - BOXES[i].y0) * sy;
        e[i].scale[2] = BOXES[i].d * sz;
        e[i].tex = i == FACE_BOX ? entity_tex(T_SKIN, T_SKIN, T_SKIN, T_FACE) : BOXES[i].tex;
        e[i].light = light;
        for (int c8 = 0; c8 < 8; c8++) {
            const float v[3] = {(c8 & 1 ? 0.5f : -0.5f) * e[i].scale[0], (c8 & 2 ? 0.5f : -0.5f) * e[i].scale[1],
                                (c8 & 4 ? 0.5f : -0.5f) * e[i].scale[2]};
            float o[3] = {0};
            quat_apply(x->q, v, o);
            low = fminf(low, e[i].pos[1] + o[1]);
        }
    }
    return low;
}

int avatar_build(const avatar_anim *a, const float feet[3], float yaw, float pitch, int dead, float light,
                 entity_instance *out, int max)
{
    if (!out || max <= 0) return 0;
    entity_instance e[AVATAR_BOXES] = {0};
    float eyes[3] = {0};
    float low = pose(a, yaw, pitch, dead, light, e, eyes, NULL);
    /* Stand on the lowest point, a heel or a toe walking: the hips rise and
     * fall because the legs do, with no bob added. */
    int n = max < AVATAR_BOXES ? max : AVATAR_BOXES;
    for (int i = 0; i < n; i++) {
        out[i] = e[i];
        out[i].pos[0] += feet[0];
        out[i].pos[1] += feet[1] - low;
        out[i].pos[2] += feet[2];
    }
    return n;
}

int avatar_build_self(const avatar_anim *a, const float eye[3], float feet_y, float yaw, float pitch, float light,
                      entity_instance *out, int max)
{
    if (!out || max <= 0) return 0;
    entity_instance e[AVATAR_BOXES] = {0};
    float eyes[3] = {0};
    float low = pose(a, yaw, pitch, 0, light, e, eyes, NULL);
    const float shift[3] = {eye[0] - eyes[0], feet_y - low, eye[2] - eyes[2]};
    int n = 0;
    for (int i = 0; i < AVATAR_BOXES && n < max; i++) {
        int b = BOXES[i].bone;
        if (b == AV_HEAD || b == AV_NECK || (b >= AV_SHOULDER_R && b < AV_HIP_R)) continue;
        out[n] = e[i];
        for (int k = 0; k < 3; k++) out[n].pos[k] += shift[k];
        n++;
    }
    return n;
}

void avatar_pose_get(const avatar_anim *a, float yaw, float pitch, int dead, avatar_pose *out)
{
    entity_instance e[AVATAR_BOXES] = {0};
    xform w[AVATAR_BONES] = {0};
    float low = pose(a, yaw, pitch, dead, 1.0f, e, out->eyes, w);
    for (int b = 0; b < AVATAR_BONES; b++) {
        for (int k = 0; k < 3; k++) out->at[b][k] = w[b].p[k];
        out->at[b][1] -= low;
        out->q[b] = w[b].q;
    }
    out->eyes[1] -= low;
}

void avatar_pose_map(const avatar_pose *p, int bone, const float at[3], const float dir[3], float at_out[3],
                     float dir_out[3])
{
    avatar_anim still = {0};
    avatar_pose ref = {0};
    avatar_pose_get(&still, 0.0f, 0.0f, 0, &ref);
    int b = bone >= 0 && bone < AVATAR_BONES ? bone : AV_CHEST;
    /* Into the bone's own frame on the standing figure, then out of it
     * where the bone is now. */
    quat inv = {-ref.q[b].x, -ref.q[b].y, -ref.q[b].z, ref.q[b].w};
    const float rel[3] = {at[0] - ref.at[b][0], at[1] - ref.at[b][1], at[2] - ref.at[b][2]};
    float local[3] = {0}, ldir[3] = {0}, o[3] = {0};
    quat_apply(inv, rel, local);
    quat_apply(inv, dir, ldir);
    quat_apply(p->q[b], local, o);
    for (int k = 0; k < 3; k++) at_out[k] = p->at[b][k] + o[k];
    quat_apply(p->q[b], ldir, dir_out);
}
