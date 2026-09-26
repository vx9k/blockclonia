#include "viewmodel.h"
#include "anim.h"
#include "block.h"
#include "item.h"

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

/* Rest pose: the pivot (the back end of the sleeve, a stand-in for the
 * shoulder pulled forward so the arm is on screen) and the arm's aim,
 * turned in toward the crosshair and raised a little. */
static const float PIVOT[3] = {0.46f, -0.38f, -0.28f};
#define REST_YAW 0.21f          /* rad, inward (toward -x) */
#define REST_PITCH 0.14f        /* rad, up */

/* Parts along the arm's own axes (-z runs from the shoulder to the hand). */
#define SLEEVE_LEN 0.5f
#define SLEEVE_W 0.12f
#define HAND_LEN 0.14f
#define HAND_W 0.11f
#define HAND_Z (-(SLEEVE_LEN + HAND_LEN * 0.5f - 0.005f)) /* overlaps the cuff by 5 mm */
#define BLOCK_SIZE 0.22f
#define CARD_SIZE 0.30f
#define CARD_THICK 0.02f

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
    entity_instance e;
    float r[3], p[3];
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
        !isfinite(vm->time)) {
        int shown = vm->shown_id;
        viewmodel_init(vm);
        vm->shown_id = shown;
    }
    vm->time = fmodf(vm->time + dt, TIME_WRAP);

    /* Equip: lower while the hand holds something other than what is
     * drawn, swap at the bottom, raise. */
    int want = in->held_id > 0 ? in->held_id : 0;
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
    if (finite_or0(in->swing) > 0.98f && t > SWING_PEAK) {
        float a = swing_amount(t);
        vm->swing_t = SWING_PEAK * (2.0f / PI_F) * asinf(clamp_range(a, 0.0f, 1.0f));
    } else if (in->swinging && t >= 1.0f) {
        vm->swing_t = 0.0f;
    }
    vm->swing_t = fminf(1.0f, fmaxf(0.0f, vm->swing_t) + dt / SWING_S);

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
    float base[3];
    base[0] = PIVOT[0] + 0.030f * bob * ss + 0.002f * rest * breath_x + 0.03f * sprint - 0.02f * eat;
    base[1] = PIVOT[1] + 0.016f * bob * s2 + 0.004f * rest * breath - 0.05f * sprint + (0.012f * chew - 0.03f) * eat;
    base[2] = PIVOT[2] + 0.04f * sprint + 0.10f * eat;

    /* The arm's aim: sprinting points it down and rolls it outward (the
     * tilt), eating turns it in and up to the mouth. */
    float yaw = REST_YAW + 0.5f * eat - 0.02f * bob * ss;
    float pitch = REST_PITCH - 0.22f * sprint + 0.35f * eat + 0.05f * eat * chew;
    float roll = -0.35f * sprint + 0.04f * bob * ss;
    quat arm = quat_mul(quat_axis(0.0f, 1.0f, 0.0f, yaw),
                        quat_mul(quat_axis(1.0f, 0.0f, 0.0f, pitch), quat_axis(0.0f, 0.0f, 1.0f, roll)));
    /* The swing: about the pivot, around an axis tilted so the hand
     * sweeps down and in toward the crosshair. */
    arm = quat_norm(quat_mul(quat_axis(-0.6f, 1.0f, 0.0f, 0.55f * a), arm));

    /* Lag rotates the whole view model about the eye (yaw is positive to
     * the right, so the view-space angle is its negative). */
    quat lag = quat_mul(quat_axis(0.0f, 1.0f, 0.0f, -vm->lag_x), quat_axis(1.0f, 0.0f, 0.0f, vm->lag_y));

    entity_instance parts[VIEWMODEL_MAX];
    int n = 0;
    const float sleeve_at[3] = {0.0f, 0.0f, -SLEEVE_LEN * 0.5f};
    const float hand_at[3] = {0.0f, 0.0f, HAND_Z};
    parts[n++] = part(lag, base, arm, sleeve_at, quat_identity(), SLEEVE_W, SLEEVE_W, SLEEVE_LEN, same_tex(T_SLEEVE),
                      light);
    parts[n++] = part(lag, base, arm, hand_at, quat_identity(), HAND_W, HAND_W, HAND_LEN, same_tex(T_SKIN), light);

    int id = vm->shown_id;
    quat flick = quat_axis(1.0f, 0.0f, 0.0f, -0.6f * a); /* the wrist leads the swing */
    if (id > 0 && item_is_block(id)) {
        /* A block rests on the fingers, turned so the top and two sides
         * face the eye. */
        const block_def *d = block_get((uint8_t)id);
        uint32_t tex = (uint32_t)d->tex[0] | (uint32_t)d->tex[2] << 8 | (uint32_t)d->tex[3] << 16;
        const float at[3] = {-0.02f, 0.12f, HAND_Z - 0.03f};
        quat own = quat_mul(flick, quat_axis(0.0f, 1.0f, 0.0f, 0.62f));
        parts[n++] = part(lag, base, arm, at, own, BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE, tex, light);
    } else if (id > 0) {
        /* Any other item is a card showing its sprite, gripped at its lower
         * end, turned toward the eye and leaning over like a held tool.
         * It is always the last instance: its texture has transparent
         * texels. */
        const float at[3] = {-0.03f, 0.14f, HAND_Z - 0.04f};
        quat own = quat_mul(flick, quat_mul(quat_axis(0.0f, 1.0f, 0.0f, -0.45f), quat_axis(0.0f, 0.0f, 1.0f, 0.35f)));
        parts[n++] = part(lag, base, arm, at, own, CARD_SIZE, CARD_SIZE, CARD_THICK, same_tex(item_get(id)->tex),
                          light);
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
