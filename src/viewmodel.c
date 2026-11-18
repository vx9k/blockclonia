#include "viewmodel.h"
#include "anim.h"
#include "block.h"
#include "item.h"
#include "itemmodel.h"
#include "torch_model.h"

#include <math.h>
#include <string.h>

/* The arm is a rigid chain hung from a shoulder pivot below and to the
 * right of the eye: every pose is a translation of the pivot plus a
 * rotation of the whole arm about it, so a swing moves the hand along an
 * arc instead of a straight line, and the held item rides on the hand.
 * View space: x right, y up, -z forward, metres. The renderer draws it with
 * its own 70 degree vertical view and a 0.02 m near plane.
 *
 * Timings are human ones: a resting adult breathes 12-16 times a minute
 * (0.25 Hz here), chews at 1-2 Hz (1.5 Hz here), and a hand swing takes
 * about 0.3 s, which is also how long interact.swing takes to ease from 1
 * to 0 (3.5 /s). */

#define PI_F 3.14159265f

/* Rest pose: the pivot (the back end of the forearm, a stand-in for the
 * shoulder pulled forward so the arm is on screen) and the arm's aim,
 * turned in toward the crosshair and raised a little. */
static const float PIVOT[3] = {0.46f, -0.38f, -0.28f};
#define REST_YAW 0.21f          /* rad, inward (toward -x) */
#define REST_PITCH 0.14f        /* rad, up */

/* Parts along the arm's own axes (-z runs from the shoulder to the hand). */
/* The arm is bare (the base layer is a singlet) and as thick as the
 * figure's: a forearm 8.5 cm across and a fist 9 cm. */
#define FOREARM_LEN 0.5f
#define FOREARM_W 0.085f
#define HAND_LEN 0.12f
#define HAND_W 0.09f
#define HAND_Z (-(FOREARM_LEN + HAND_LEN * 0.5f - 0.005f)) /* overlaps the wrist by 5 mm */
#define BLOCK_SIZE 0.18f
#define PALM_SIZE 0.14f /* m: what an item in the palm is scaled to, within limits */
#define TOOL_HELD 0.5f  /* a tool's model in the hand, of its real size */
#define KNIFE_HELD 1.5f
#define TORCH_HELD 0.5f /* the torch model's size in the hand */

/* Timings, s. */
#define SWING_S 0.29f
#define SWING_PEAK 0.35f        /* fraction of a swing spent going down */
#define LOWER_S 0.12f           /* equip: lower, swap, raise */
#define RAISE_S 0.16f
#define LAG_TAU 0.06f           /* the arm trails the head like a first-order follower */
#define LAG_MAX 0.12f           /* rad */
#define BREATH_HZ 0.25f
#define CHEW_HZ 1.5f
#define TIME_WRAP 3600.0f       /* whole periods of every rhythm above */

/* The renderer's view-model pass shows 35 degrees below the crosshair. */
#define SCREEN_SOFT 0.45f       /* rad (26 deg): below this the arm is eased back up */
#define SCREEN_GIVE 0.11f       /* rad: it never gets more than this further (32 deg) */

/* Two hands: the arms hang from shoulders 21 cm below the eye and a little
 * behind it, 19 cm to either side, and each reaches its grip by two-bone
 * IK. Lengths are the figure's (avatar.c): an upper arm of 0.186 and a
 * forearm of 0.146 of its 1.75 m height. */
static const float SHOULDER[2][3] = {{0.19f, -0.21f, 0.05f}, {-0.19f, -0.21f, 0.05f}};
#define UPPER_ARM 0.32f
#define LOWER_ARM 0.26f
#define PROTRACT 0.08f /* m the shoulder blade can slide forward to reach further */

/* How the item takes the hands: one, or two on a haft swung in a chop
 * (pick, axe) or thrust in a dig (shovel), or two on the rifle. */
enum { HOLD_ONE, HOLD_CHOP, HOLD_DIG, HOLD_GUN };
#define CHOP_S 0.8f
#define DIG_S 0.7f
#define CHOP_REJOIN 0.92f /* a new hit after this much of a chop (all but back at rest) starts the next */

/* A haft in both hands: the left hand's grip (the knob end) in view space,
 * the haft's angle above straight ahead and its turn to the right, in
 * degrees, and how far up the haft the right hand holds it, in metres. */
enum { K_X, K_Y, K_Z, K_PITCH, K_YAW, K_GRIP, KEY_CH };
typedef struct {
    float t; /* fraction of the action */
    float v[KEY_CH];
} hold_key;

/* The chop (after the design: 0.18 s wind-up, a 0.1 s strike, a short stop
 * at impact, then 0.43 s back): carried at 55 degrees off to the right, cocked up past
 * vertical with the right hand slid up toward the head, then whipped down
 * with that hand sliding back to the left, which drives the head. The edge
 * leads, so it lands just under the crosshair edge first. */
/* clang-format off */
static const hold_key CHOP[] = {
    {0.00f, {0.10f, -0.27f, -0.36f,  55.0f, 25.0f, 0.24f}},
    {0.22f, {0.22f, -0.16f, -0.30f, 100.0f, 32.0f, 0.40f}},
    {0.28f, {0.22f, -0.15f, -0.30f, 103.0f, 32.0f, 0.40f}},
    {0.40f, {0.05f, -0.27f, -0.40f,  22.0f,  7.0f, 0.12f}},
    {0.46f, {0.05f, -0.26f, -0.39f,  26.0f,  7.0f, 0.12f}},
    {1.00f, {0.10f, -0.27f, -0.36f,  55.0f, 25.0f, 0.24f}},
};
/* The dig: drawn back, driven forward and down into the ground, levered up. */
static const hold_key DIG[] = {
    {0.00f, {0.08f, -0.27f, -0.34f, 40.0f, 22.0f, 0.30f}},
    {0.25f, {0.11f, -0.23f, -0.26f, 50.0f, 22.0f, 0.34f}},
    {0.42f, {0.02f, -0.31f, -0.50f,  8.0f,  8.0f, 0.30f}},
    {0.50f, {0.02f, -0.31f, -0.51f,  6.0f,  8.0f, 0.30f}},
    {0.70f, {0.05f, -0.26f, -0.42f, 32.0f, 14.0f, 0.30f}},
    {1.00f, {0.08f, -0.27f, -0.34f, 40.0f, 22.0f, 0.30f}},
};
/* clang-format on */
#define NKEYS(k) ((int)(sizeof(k) / sizeof((k)[0])))

/* The rifle (itemmodel.c: muzzle up its y, top toward -x). Down the sights
 * the eye sits on the line over the rear notch and the front post, a cheek
 * weld 0.3 m behind the notch; at the hip that point is low and right, the
 * rifle turned in a little and canted so its side shows. */
static const float GUN_EYE[3] = {-0.092f, 0.23f, 0.0f};
static const float GUN_HIP[3] = {0.12f, -0.15f, -0.22f};
/* Raising the rifle is a mount, not a slide: an underdamped spring (2.4 Hz,
 * damping 0.6) carries it, overshooting by about a tenth and settling in
 * a third of a second, the way a body brings a weight up to the cheek and
 * stops it there. */
#define MOUNT_HZ 2.4f
#define MOUNT_DAMP 0.6f
static const float GUN_HAND[2][3] = {{0.035f, 0.33f, 0.0f}, {0.04f, 0.82f, 0.0f}}; /* wrist of the stock, fore-end */
/* The bolt turns about the bore's axis and draws back along it. */
#define BOLT_TURN 1.05f  /* rad, 60 degrees: the locking lugs clear */
#define BOLT_THROW 0.09f /* m, enough to eject a 7 cm case and feed the next */
/* The barrel's end, on the bore's axis (itemmodel.c's rifle). */
static const float GUN_MUZZLE[3] = {-0.034f, 1.11f, 0.0f};

/* ---- helpers */

static float finite_or0(float v) { return isfinite(v) ? v : 0.0f; }

static float clamp_range(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* How far along its arc the hand is (0 rest .. 1 lowest) at progress t:
 * a quick eased drop (sine, so it leaves rest at full speed and stops at
 * the bottom) and a slower smooth return. */
static float swing_amount(float t)
{
    if (t <= 0.0f || t >= 1.0f) return 0.0f;
    if (t < SWING_PEAK) return sinf(0.5f * PI_F * t / SWING_PEAK);
    return 0.5f + 0.5f * cosf(PI_F * (t - SWING_PEAK) / (1.0f - SWING_PEAK));
}

static uint32_t same_tex(int layer) { return (uint32_t)layer * 0x010101u; }

static int hold_of(item_id id)
{
    int kind = 0, tier = 0;
    if (id == I_RIFLE) return HOLD_GUN;
    if (!item_tool(id, &kind, &tier)) return HOLD_ONE;
    if (kind == TOOL_SHOVEL) return HOLD_DIG;
    return kind == TOOL_PICKAXE || kind == TOOL_AXE ? HOLD_CHOP : HOLD_ONE;
}

int viewmodel_hands(item_id id) { return hold_of(id) == HOLD_ONE ? 1 : 2; }

/* One channel of a hold's keyframes at t: a monotone cubic (Fritsch-
 * Carlson), so the haft never overshoots a pose, flat at both ends. */
static float key_at(const hold_key *k, int n, int ch, float t)
{
    t = clamp_range(t, k[0].t, k[n - 1].t);
    int i = 0;
    while (i < n - 2 && t > k[i + 1].t) i++;
    float d[3] = {0}; /* slopes before, of and after the piece i..i+1 */
    for (int j = 0; j < 3; j++) {
        int a = i - 1 + j;
        if (a >= 0 && a <= n - 2) d[j] = (k[a + 1].v[ch] - k[a].v[ch]) / (k[a + 1].t - k[a].t);
    }
    float m0 = d[0] * d[1] <= 0.0f ? 0.0f : 2.0f * d[0] * d[1] / (d[0] + d[1]);
    float m1 = d[1] * d[2] <= 0.0f ? 0.0f : 2.0f * d[1] * d[2] / (d[1] + d[2]);
    float h = k[i + 1].t - k[i].t, u = (t - k[i].t) / h, u2 = u * u, u3 = u2 * u;
    return (2.0f * u3 - 3.0f * u2 + 1.0f) * k[i].v[ch] + (u3 - 2.0f * u2 + u) * h * m0 +
           (-2.0f * u3 + 3.0f * u2) * k[i + 1].v[ch] + (u3 - u2) * h * m1;
}

static float vdot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static float vnorm(float v[3])
{
    float l = sqrtf(vdot(v, v));
    if (l > 1e-6f)
        for (int i = 0; i < 3; i++) v[i] /= l;
    return l;
}

/* The turn taking -z (a part's long axis) onto the unit vector d. */
static quat quat_from_fwd(const float d[3])
{
    if (d[2] > 0.9999f) return quat_axis(1.0f, 0.0f, 0.0f, PI_F);
    /* half-way quaternion: axis (-z x d), w = 1 + (-z . d) */
    return quat_norm((quat){d[1], -d[0], 0.0f, 1.0f - d[2]});
}

static quat quat_blend(quat a, quat b, float t)
{
    float s = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f ? -1.0f : 1.0f; /* the short way round */
    return quat_norm((quat){a.x + (s * b.x - a.x) * t, a.y + (s * b.y - a.y) * t, a.z + (s * b.z - a.z) * t,
                            a.w + (s * b.w - a.w) * t});
}

/* Two-bone IK: the elbow for a hand whose grip (UPPER_ARM + LOWER_ARM
 * from the shoulder at full stretch) is at p, bent toward pole. Out of
 * reach, the shoulder slides forward up to PROTRACT and the arm straightens. */
static void ik_elbow(const float shoulder[3], const float p[3], const float pole[3], float elbow[3])
{
    float s[3] = {shoulder[0], shoulder[1], shoulder[2]};
    float u[3] = {p[0] - s[0], p[1] - s[1], p[2] - s[2]};
    float want = vnorm(u);
    const float l1 = UPPER_ARM, l2 = LOWER_ARM;
    float over = fminf(fmaxf(want - (l1 + l2 - 1e-3f), 0.0f), PROTRACT);
    for (int i = 0; i < 3; i++) s[i] += u[i] * over;
    float d = clamp_range(want - over, fabsf(l1 - l2) + 1e-3f, l1 + l2 - 1e-3f);
    float w[3] = {0};
    float pu = vdot(pole, u);
    for (int i = 0; i < 3; i++) w[i] = pole[i] - u[i] * pu;
    vnorm(w);
    float ca = (l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), sa = sqrtf(fmaxf(0.0f, 1.0f - ca * ca));
    for (int i = 0; i < 3; i++) elbow[i] = s[i] + u[i] * l1 * ca + w[i] * l1 * sa;
}

static entity_instance cube(const float pos[3], quat rot, float sx, float sy, float sz, uint32_t tex, float light)
{
    entity_instance e = {0};
    memcpy(e.pos, pos, sizeof e.pos);
    entity_set_rot(&e, rot);
    e.scale[0] = sx;
    e.scale[1] = sy;
    e.scale[2] = sz;
    e.tex = tex;
    e.light = light;
    return e;
}

/* An arm reaching the grip at p: the forearm from the elbow to the wrist
 * and the fist round the grip, along the same line. */
static int reach(entity_instance *out, int side, const float p[3], float light)
{
    /* Elbows drop and flare out, the right one more for a rifle's wrist. */
    static const float POLE[2][3] = {{0.7f, -0.7f, 0.15f}, {-0.6f, -0.78f, 0.15f}};
    float e[3] = {0}, dir[3] = {p[0], p[1], p[2]};
    ik_elbow(SHOULDER[side], p, POLE[side], e);
    for (int i = 0; i < 3; i++) dir[i] -= e[i];
    float len = vnorm(dir);
    float fore = fmaxf(len - HAND_LEN * 0.5f, 0.01f);
    float mid[3] = {0};
    for (int i = 0; i < 3; i++) mid[i] = e[i] + dir[i] * fore * 0.5f;
    quat r = quat_from_fwd(dir);
    out[0] = cube(mid, r, FOREARM_W, FOREARM_W, fore, same_tex(T_SKIN), light);
    out[1] = cube(p, r, HAND_W, HAND_W, HAND_LEN, same_tex(T_SKIN), light);
    return 2;
}

static quat inst_rot(const entity_instance *e) { return (quat){e->rot[0], e->rot[1], e->rot[2], e->rot[3]}; }

static void vadd(float out[3], const float a[3], const float b[3])
{
    for (int i = 0; i < 3; i++) out[i] = a[i] + b[i];
}

/* One cube of the chain: centre = lag * (base + arm * local), orientation
 * lag * arm * own. */
static entity_instance part(quat lag, const float base[3], quat arm, const float local[3], quat own, float sx, float sy,
                            float sz, uint32_t tex, float light)
{
    entity_instance e = {0};
    float r[3] = {0}, p[3] = {0};
    quat_apply(arm, local, r);
    vadd(p, base, r);
    quat_apply(lag, p, e.pos);
    e.tex = tex;
    entity_set_rot(&e, quat_norm(quat_mul(lag, quat_mul(arm, own))));
    e.scale[0] = sx;
    e.scale[1] = sy;
    e.scale[2] = sz;
    e.light = light;
    return e;
}

/* ---- state */

void viewmodel_init(viewmodel *vm)
{
    memset(vm, 0, sizeof *vm);
    vm->swing_t = 1.0f; /* no swing playing; equip 0: the arm rises into view */
}

static void advance(viewmodel *vm, const viewmodel_input *in, float dt)
{
    /* A NaN that got in once would stay forever; start clean instead. */
    if (!isfinite(vm->equip) || !isfinite(vm->swing_t) || !isfinite(vm->lag_x) || !isfinite(vm->lag_y) ||
        !isfinite(vm->time) || !isfinite(vm->aim_x) || !isfinite(vm->aim_v)) {
        item_id shown = vm->shown_id;
        viewmodel_init(vm);
        vm->shown_id = shown;
    }
    vm->time = fmodf(vm->time + dt, TIME_WRAP);

    /* Equip: lower while the hand holds something other than what is
     * drawn, swap at the bottom, raise. */
    item_id want = item_valid(in->held_id) ? in->held_id : 0; /* garbage shows the bare hand */
    if (want != vm->shown_id) {
        vm->equip -= dt / LOWER_S;
        if (vm->equip <= 0.0f) {
            vm->equip = 0.0f;
            vm->shown_id = want;
        }
    } else {
        vm->equip = fminf(1.0f, vm->equip + dt / RAISE_S);
    }

    /* Swing: interact sets swing to 1 on each action. A new action while
     * the hand is on its way back rejoins the downward stroke at the same
     * height, so repeated hits never jump; holding the button on a block
     * loops the swing. */
    float t = vm->swing_t;
    int hold = hold_of(vm->shown_id);
    if (hold == HOLD_CHOP || hold == HOLD_DIG) {
        /* A chop runs its course; a hit near its end starts the next, so
         * holding the button on a block chops on without a jump. */
        if ((finite_or0(in->swing) > 0.98f || in->swinging) && t >= CHOP_REJOIN) vm->swing_t = 0.0f;
        vm->swing_t = fminf(1.0f, fmaxf(0.0f, vm->swing_t) + dt / (hold == HOLD_CHOP ? CHOP_S : DIG_S));
    } else {
        if (finite_or0(in->swing) > 0.98f && t > SWING_PEAK) {
            float a = swing_amount(t);
            vm->swing_t = SWING_PEAK * (2.0f / PI_F) * asinf(clamp_range(a, 0.0f, 1.0f));
        } else if (in->swinging && t >= 1.0f) {
            vm->swing_t = 0.0f;
        }
        vm->swing_t = fminf(1.0f, fmaxf(0.0f, vm->swing_t) + dt / SWING_S);
    }

    /* The mount follows the aim on its spring, in steps short enough to
     * stay stable at any frame rate; another item drops it at once. */
    if (hold == HOLD_GUN) {
        vm->bolt = finite_or0(in->bolt);
        float target = anim_clamp01(finite_or0(in->aim)), w = 2.0f * PI_F * MOUNT_HZ;
        int steps = (int)ceilf(dt * 240.0f);
        float h = steps > 0 ? dt / (float)steps : 0.0f;
        for (int i = 0; i < steps; i++) {
            vm->aim_v += (w * w * (target - vm->aim_x) - 2.0f * MOUNT_DAMP * w * vm->aim_v) * h;
            vm->aim_x += vm->aim_v * h;
        }
    } else {
        vm->aim_x = vm->aim_v = 0.0f;
    }

    /* Mouse lag: a first-order follower driven by the look rate w settles
     * at an angle -w * tau behind the view, and eases back when the look
     * stops. */
    if (dt > 1e-4f) {
        float wx = finite_or0(in->look_dx) / dt, wy = finite_or0(in->look_dy) / dt;
        float tx = clamp_range(-wx * LAG_TAU, -LAG_MAX, LAG_MAX), ty = clamp_range(-wy * LAG_TAU, -LAG_MAX, LAG_MAX);
        vm->lag_x = clamp_range(anim_approach(vm->lag_x, tx, 1.0f / LAG_TAU, dt), -LAG_MAX, LAG_MAX);
        vm->lag_y = clamp_range(anim_approach(vm->lag_y, ty, 1.0f / LAG_TAU, dt), -LAG_MAX, LAG_MAX);
    }
}

/* ---- pose */

/* Where the rifle sits at aim (0 hip .. 1 sights): its turn and where the
 * model's GUN_EYE point goes, in view space. kick is the recoil (0..1),
 * drift the slow wander down the sights (radians), sway the stride's. */
static void gun_place(float aim, float sprint, float kick, float drift_x, float drift_y, const float sway[3], quat *rot,
                      float at[3])
{
    quat ads = quat_mul(quat_axis(0.0f, 1.0f, 0.0f, 0.5f * PI_F), quat_axis(0.0f, 0.0f, 1.0f, -0.5f * PI_F));
    /* At the hip it points ahead and in a little, canted to show its
     * side; running, it's carried muzzle down. */
    quat hip = quat_mul(quat_axis(0.0f, 1.0f, 0.0f, 0.07f), quat_mul(quat_axis(0.0f, 0.0f, 1.0f, 0.18f), ads));
    hip = quat_mul(quat_axis(1.0f, 0.0f, 0.0f, -0.35f * sprint), hip);
    /* On the way up the rifle swings through an arc below the line,
     * canted, butt first, the muzzle coming onto the mark last. */
    float mount = sinf(PI_F * anim_clamp01(aim));
    quat swing = quat_mul(quat_axis(0.0f, 1.0f, 0.0f, drift_x),
                          quat_mul(quat_axis(1.0f, 0.0f, 0.0f, drift_y - 0.09f * mount),
                                   quat_axis(0.0f, 0.0f, 1.0f, 0.14f * mount)));
    /* Recoil: the muzzle climbs and the butt drives back into the shoulder. */
    *rot = quat_mul(quat_axis(1.0f, 0.0f, 0.0f, 0.10f * kick), quat_mul(swing, quat_blend(hip, ads, aim)));
    float settle = 1.0f - 0.8f * aim;
    for (int i = 0; i < 3; i++) at[i] = GUN_HIP[i] * (1.0f - aim) + sway[i] * settle;
    at[0] += 0.015f * mount;
    at[1] -= 0.035f * mount;
    at[1] += 0.008f * kick - 0.03f * sprint * (1.0f - aim);
    at[2] += 0.035f * kick;
}

/* 0 before a, 1 after b, smooth between. */
static float ease_in(float t, float a, float b)
{
    float x = clamp_range((t - a) / (b - a), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

void viewmodel_bolt(float phase, float *to_knob, float *lift, float *slide)
{
    *to_knob = *lift = *slide = 0.0f;
    if (!isfinite(phase) || phase <= 0.0f || phase >= 1.0f) return;
    /* The first sixth is the recoil settling with the hand still on the
     * wrist; then the hand goes to the knob, turns it up, draws the bolt
     * back to throw the case, runs it home, turns it down and returns. */
    *to_knob = ease_in(phase, 0.17f, 0.30f) - ease_in(phase, 0.85f, 1.0f);
    *lift = ease_in(phase, 0.30f, 0.42f) - ease_in(phase, 0.74f, 0.85f);
    *slide = ease_in(phase, 0.42f, 0.58f) - ease_in(phase, 0.58f, 0.74f);
}

void viewmodel_muzzle(float aim, float pos[3], float dir[3])
{
    const float zero[3] = {0}, up[3] = {0.0f, 1.0f, 0.0f};
    float a = clamp_range(finite_or0(aim), 0.0f, 1.0f), at[3] = {0};
    quat rot = {0};
    gun_place(a, 0.0f, 0.0f, 0.0f, 0.0f, zero, &rot, at);
    const float d[3] = {GUN_MUZZLE[0] - GUN_EYE[0], GUN_MUZZLE[1] - GUN_EYE[1], GUN_MUZZLE[2] - GUN_EYE[2]};
    float r[3] = {0};
    quat_apply(rot, d, r);
    vadd(pos, at, r);
    quat_apply(rot, up, dir);
}

/* Both hands on a haft or the rifle, in view space before the mouse lag
 * and the equip dip. sway is the stride's and the breath's drift. Returns
 * the parts written: both arms, then the item. */
static int two_hands(const viewmodel *vm, int hold, float aim, float sprint, const float sway[3], float light,
                     entity_instance *out)
{
    item_model mdl = {0};
    if (!itemmodel_build(vm->shown_id, &mdl)) return 0;
    float at[3] = {0}, origin[3] = {0}, grip[2][3] = {{0}};
    quat rot = {0};
    if (hold == HOLD_GUN) {
        float kick = swing_amount(vm->swing_t); /* a shot sets the swing going */
        /* Down the sights it drifts in a slow figure of eight and with each
         * breath, a milliradian or two, as a standing shooter's does. */
        float held = anim_clamp01(aim), t = vm->time;
        float drift_x = 0.0016f * sinf(2.0f * PI_F * 0.21f * t + 1.0f) * held;
        float drift_y = (0.0012f * sinf(2.0f * PI_F * 0.42f * t) + 0.0008f * sinf(2.0f * PI_F * BREATH_HZ * t)) * held;
        gun_place(aim, sprint, kick, drift_x, drift_y, sway, &rot, at);
        memcpy(origin, GUN_EYE, sizeof origin);
        float to_knob = 0.0f, lift = 0.0f, slide = 0.0f, hand[2][3] = {{0}};
        memcpy(hand, GUN_HAND, sizeof hand);
        viewmodel_bolt(vm->bolt, &to_knob, &lift, &slide);
        if (mdl.bolt > 0 && mdl.bolt + 1 < mdl.n) {
            /* The handle turns up about the bore (its top is -x) and draws
             * back toward the butt; the right hand follows the knob. */
            quat turn = quat_axis(0.0f, 1.0f, 0.0f, -BOLT_TURN * lift);
            for (int b = mdl.bolt; b < mdl.bolt + 2; b++) {
                im_box *bx = &mdl.box[b];
                const float rel[3] = {bx->pos[0] - GUN_MUZZLE[0], 0.0f, bx->pos[2] - GUN_MUZZLE[2]};
                float r[3] = {0};
                quat_apply(turn, rel, r);
                bx->pos[0] = GUN_MUZZLE[0] + r[0];
                bx->pos[1] -= BOLT_THROW * slide;
                bx->pos[2] = GUN_MUZZLE[2] + r[2];
                bx->rot = quat_norm(quat_mul(turn, bx->rot));
            }
            /* The rifle comes down off the cheek, forward and canted to
             * the right while the hand works it, so the arm passes under
             * the eye instead of across it. */
            rot = quat_mul(quat_axis(0.0f, 0.0f, 1.0f, -0.3f * to_knob), rot);
            at[0] += 0.03f * to_knob;
            at[1] -= 0.13f * to_knob;
            at[2] -= 0.08f * to_knob;
            const float *knob = mdl.box[mdl.bolt + 1].pos;
            for (int i = 0; i < 3; i++) hand[0][i] += (knob[i] - hand[0][i]) * to_knob;
        }
        for (int s = 0; s < 2; s++) {
            const float d[3] = {hand[s][0] - GUN_EYE[0], hand[s][1] - GUN_EYE[1], hand[s][2] - GUN_EYE[2]};
            float r[3] = {0};
            quat_apply(rot, d, r);
            vadd(grip[s], at, r);
        }
    } else {
        const hold_key *k = hold == HOLD_CHOP ? CHOP : DIG;
        int nk = hold == HOLD_CHOP ? NKEYS(CHOP) : NKEYS(DIG);
        float v[KEY_CH] = {0};
        for (int c = 0; c < KEY_CH; c++) v[c] = key_at(k, nk, c, vm->swing_t);
        const float deg = PI_F / 180.0f;
        quat haft = quat_mul(quat_axis(0.0f, 1.0f, 0.0f, -v[K_YAW] * deg),
                             quat_axis(1.0f, 0.0f, 0.0f, (v[K_PITCH] - 90.0f) * deg - 0.35f * sprint));
        /* A pick's or an axe's edge (the model's +x) leads the swing,
         * turned in toward the crosshair so the head shows its profile; a
         * shovel's blade faces up, ready to take a load. */
        rot = hold == HOLD_CHOP ? quat_mul(haft, quat_axis(0.0f, 1.0f, 0.0f, 0.72f * PI_F)) : haft;
        const float up[3] = {0.0f, 1.0f, 0.0f};
        float dir[3] = {0};
        quat_apply(haft, up, dir);
        for (int i = 0; i < 3; i++) at[i] = v[K_X + i] + sway[i];
        at[1] -= 0.04f * sprint;
        memcpy(origin, mdl.grip, sizeof origin);
        /* The left hand holds the knob end; the right slides along the haft. */
        memcpy(grip[1], at, sizeof grip[1]);
        for (int i = 0; i < 3; i++) grip[0][i] = at[i] + dir[i] * v[K_GRIP];
    }
    entity_instance im[IM_MAX_BOXES] = {0};
    int k = itemmodel_place(&mdl, origin, at, rot, 1.0f, light, im, IM_MAX_BOXES);
    int n = 0;
    for (int s = 0; s < 2; s++) n += reach(out + n, s, grip[s], light);
    for (int i = 0; i < k && n < VIEWMODEL_MAX; i++) out[n++] = im[i];
    return n;
}

int viewmodel_build(viewmodel *vm, const viewmodel_input *in, entity_instance *out, int max)
{
    float dt = finite_or0(in->dt);
    dt = clamp_range(dt, 0.0f, 0.25f); /* a long hitch should not fling the arm */
    advance(vm, in, dt);

    float lower = 1.0f - ease_in_out(vm->equip);
    float a = swing_amount(vm->swing_t);
    float sprint = anim_clamp01(finite_or0(in->sprint));
    float bob = anim_clamp01(finite_or0(in->bob));
    float stride = fmodf(finite_or0(in->stride), 2.0f * PI_F);
    float light = in->light > 0.0f && isfinite(in->light) ? fminf(in->light, 4.0f) : 1.0f; /* 0: unset */

    /* Eating: in->eating is the action's progress; the hand comes up over
     * the first fifth and goes back down over the last. */
    float ep = anim_clamp01(finite_or0(in->eating));
    float eat = ep > 0.0f ? ease_in_out(fminf(ep, 1.0f - ep) * 5.0f) : 0.0f;

    float t = vm->time;
    float breath = sinf(2.0f * PI_F * BREATH_HZ * t);
    float breath_x = sinf(PI_F * BREATH_HZ * t);
    float chew = sinf(2.0f * PI_F * CHEW_HZ * t);
    float ss = sinf(stride), s2 = sinf(2.0f * stride);
    float rest = 1.0f - bob; /* breathing shows when standing still */

    /* The pivot. Walking traces a figure-eight (a 1:2 Lissajous: one sway
     * per stride, two dips, one per foot); sprinting pulls the arm down,
     * out and back. */
    float base[3] = {0};
    base[0] = PIVOT[0] + 0.045f * bob * ss + 0.002f * rest * breath_x + 0.03f * sprint - 0.02f * eat - 0.08f * a;
    base[1] = PIVOT[1] + 0.026f * bob * s2 + 0.004f * rest * breath - 0.05f * sprint + (0.012f * chew - 0.03f) * eat -
              0.04f * a;
    /* A hit is thrown from the shoulder: it reaches forward and in toward
     * the crosshair as it drops, so the arm keeps its line instead of
     * swinging flat across the view. */
    base[2] = PIVOT[2] + 0.04f * sprint + 0.06f * eat - 0.06f * a;

    /* The arm's aim: sprinting points it down and rolls it outward (the
     * tilt), eating turns it in and up to the mouth. */
    float yaw = REST_YAW + 0.5f * eat - 0.02f * bob * ss;
    float pitch = REST_PITCH - 0.22f * sprint + 0.35f * eat + 0.05f * eat * chew + 0.03f * bob * s2;
    float roll = -0.35f * sprint + 0.04f * bob * ss;
    quat arm = quat_mul(quat_axis(0.0f, 1.0f, 0.0f, yaw),
                        quat_mul(quat_axis(1.0f, 0.0f, 0.0f, pitch), quat_axis(0.0f, 0.0f, 1.0f, roll)));
    /* The swing: about the pivot, mostly a downward chop (pitch) with a
     * little turn in toward the crosshair, so the hand drops along an arc
     * instead of sweeping sideways. */
    arm = quat_norm(quat_mul(quat_axis(1.0f, -0.3f, 0.0f, -0.75f * a), arm));

    /* Lag rotates the whole view model about the eye (yaw is positive to
     * the right, so the view-space angle is its negative). */
    int hold = hold_of(vm->shown_id);
    /* The mount may overshoot a little past 1; that's the settle. */
    float aim = hold == HOLD_GUN ? clamp_range(vm->aim_x, -0.2f, 1.2f) : 0.0f;
    float steady = 1.0f - 0.7f * anim_clamp01(aim); /* down the sights the rifle is held still against the cheek */
    quat lag = quat_mul(quat_axis(0.0f, 1.0f, 0.0f, -vm->lag_x * steady),
                        quat_axis(1.0f, 0.0f, 0.0f, vm->lag_y * steady));

    if (hold != HOLD_ONE) {
        const float sway[3] = {0.02f * bob * ss + 0.002f * rest * breath_x, 0.014f * bob * s2 + 0.004f * rest * breath,
                               0.0f};
        entity_instance two[VIEWMODEL_MAX] = {0};
        int k = two_hands(vm, hold, aim, sprint, sway, light, two);
        for (int i = 0; i < k; i++) {
            float p[3] = {two[i].pos[0], two[i].pos[1], two[i].pos[2]};
            quat_apply(lag, p, two[i].pos);
            two[i].pos[1] -= 0.7f * lower;
            entity_set_rot(&two[i], quat_norm(quat_mul(lag, inst_rot(&two[i]))));
        }
        if (!out || max <= 0) return 0;
        if (k > max) k = max;
        memcpy(out, two, (size_t)k * sizeof two[0]);
        return k;
    }

    entity_instance parts[VIEWMODEL_MAX] = {0};
    int n = 0;
    const float forearm_at[3] = {0.0f, 0.0f, -FOREARM_LEN * 0.5f};
    const float hand_at[3] = {0.0f, 0.0f, HAND_Z};
    parts[n++] = part(lag, base, arm, forearm_at, quat_identity(), FOREARM_W, FOREARM_W, FOREARM_LEN, same_tex(T_SKIN),
                      light);
    parts[n++] = part(lag, base, arm, hand_at, quat_identity(), HAND_W, HAND_W, HAND_LEN, same_tex(T_SKIN), light);

    item_id id = vm->shown_id;
    quat flick = quat_axis(1.0f, 0.0f, 0.0f, -0.6f * a); /* the wrist leads the swing */
    if (id == B_TORCH) {
        /* A torch is gripped upright in the fist, its foot below the hand
         * and leaning a little away, flame in view. */
        entity_instance tm[TORCH_PARTS] = {0};
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        quat own = quat_mul(flick, quat_mul(quat_axis(0.0f, 0.0f, 1.0f, 0.18f), quat_axis(1.0f, 0.0f, 0.0f, -0.25f)));
        int k = torch_model(zero, quat_identity(), TORCH_HELD, (double)vm->time, 7u, light, tm);
        for (int i = 0; i < k; i++) {
            float off[3] = {0.0f, 0.0f, 0.0f};
            quat_apply(own, tm[i].pos, off);
            const float at[3] = {off[0], off[1] - 0.12f, off[2] + HAND_Z - 0.02f};
            quat pr = {tm[i].rot[0], tm[i].rot[1], tm[i].rot[2], tm[i].rot[3]};
            parts[n++] = part(lag, base, arm, at, quat_mul(own, pr), tm[i].scale[0], tm[i].scale[1], tm[i].scale[2],
                              tm[i].tex, tm[i].light);
        }
    } else if (id > 0 && item_is_block(id)) {
        /* A block rests on the fingers, turned so the top and two sides
         * face the eye. */
        const block_def *d = block_get((uint8_t)id);
        uint32_t tex = (uint32_t)d->tex[0] | (uint32_t)d->tex[2] << 8 | (uint32_t)d->tex[3] << 16;
        const float at[3] = {-0.02f, 0.10f, HAND_Z - 0.03f};
        quat own = quat_mul(flick, quat_axis(0.0f, 1.0f, 0.0f, 0.62f));
        parts[n++] = part(lag, base, arm, at, own, BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE, tex, light);
    } else if (id > 0) {
        /* Any other item is its model (itemmodel.h). A blade points ahead; a
         * tool is gripped at the foot of its handle, leaning away with its
         * head up; anything else rests on the fingers, a small thing shown a
         * little larger and a big one smaller, so all read at a glance. */
        item_model mdl = {0};
        if (itemmodel_build(id, &mdl)) {
            quat own = quat_mul(flick, quat_axis(0.0f, 1.0f, 0.0f, 0.62f));
            float at[3] = {-0.02f, 0.10f, HAND_Z - 0.03f};
            float size = fminf(fmaxf(PALM_SIZE / itemmodel_extent(&mdl), 0.7f), 2.5f);
            int kind = 0, tier = 0;
            if (item_tool(id, &kind, &tier) && (kind == TOOL_KNIFE || kind == TOOL_SWORD)) {
                /* A blade points on from the fist along the arm and a little
                 * up, its edge down: a sword at its real size, a knife larger
                 * so it reads at arm's length. */
                own = quat_mul(flick,
                               quat_mul(quat_axis(1.0f, 0.0f, 0.0f, -1.2f), quat_axis(0.0f, 1.0f, 0.0f, 0.5f * PI_F)));
                at[0] = 0.0f;
                at[1] = 0.0f;
                at[2] = HAND_Z;
                size = kind == TOOL_KNIFE ? KNIFE_HELD : 1.0f;
            } else if (mdl.hold == IM_HOLD_TOOL) {
                own = quat_mul(flick, quat_mul(quat_axis(0.0f, 0.0f, 1.0f, 0.2f), quat_axis(1.0f, 0.0f, 0.0f, -0.55f)));
                at[0] = 0.0f;
                at[1] = 0.01f;
                at[2] = HAND_Z - 0.01f;
                size = TOOL_HELD;
            }
            entity_instance im[IM_MAX_BOXES] = {0};
            const float zero[3] = {0.0f, 0.0f, 0.0f};
            int k = itemmodel_place(&mdl, mdl.grip, zero, own, size, light, im, IM_MAX_BOXES);
            for (int i = 0; i < k && n < VIEWMODEL_MAX; i++) {
                const float pos[3] = {im[i].pos[0] + at[0], im[i].pos[1] + at[1], im[i].pos[2] + at[2]};
                parts[n] = part(lag, base, arm, pos, inst_rot(&im[i]), im[i].scale[0], im[i].scale[1], im[i].scale[2],
                                im[i].tex, im[i].light);
                parts[n++].tint = im[i].tint;
            }
        }
    }

    /* Swing, sprint, bob and lag can add up to more than the frame has
     * below the crosshair. Past SCREEN_SOFT the lowest of hand and item is
     * eased toward SCREEN_SOFT + SCREEN_GIVE (under the 35 degree edge) by
     * turning everything up about the eye. */
    float low = -1.0f;
    for (int i = 1; i < n; i++) low = fmaxf(low, atan2f(-parts[i].pos[1], -parts[i].pos[2]));
    float lift = 0.0f;
    if (low > SCREEN_SOFT) lift = low - SCREEN_SOFT - SCREEN_GIVE * (1.0f - expf(-(low - SCREEN_SOFT) / SCREEN_GIVE));
    /* Equip comes last, since it must leave the frame: the arm drops
     * straight down until even the top of a held card is below the edge. */
    quat post = quat_axis(1.0f, 0.0f, 0.0f, lift);
    for (int i = 0; i < n; i++) {
        float p[3] = {parts[i].pos[0], parts[i].pos[1], parts[i].pos[2]};
        quat_apply(post, p, parts[i].pos);
        parts[i].pos[1] -= 0.7f * lower;
        entity_set_rot(&parts[i], quat_norm(quat_mul(post, inst_rot(&parts[i]))));
    }

    if (!out || max <= 0) return 0;
    if (n > max) n = max;
    memcpy(out, parts, (size_t)n * sizeof parts[0]);
    return n;
}
