/* The view model and the other animation state (viewmodel.c). */
#include "item.h"
#include "itemmodel.h"
#include "test_util.h"
#include "viewmodel.h"

#include <math.h>
#include <string.h>

#define VM_DT (1.0f / 60.0f)
#define TAN_HALF_VFOV 0.7002f /* tan(35 deg): the view-model pass draws a 70 degree vertical view */
#define NEAR_Z (-0.02f)       /* its near plane */
#define HAND VIEWMODEL_PALM    /* instance order: the arm's parts, then the held item */
#define HELD VIEWMODEL_ARM_PARTS

static viewmodel_input vm_input(item_id held)
{
    viewmodel_input in = {0};
    memset(&in, 0, sizeof in);
    in.held_id = held;
    in.light = 1.0f;
    in.dt = VM_DT;
    return in;
}

/* Steps the view model at 60 Hz with a fixed input; returns the last count. */
static int vm_run(viewmodel *vm, const viewmodel_input *in, float seconds, entity_instance *out)
{
    int n = 0;
    int steps = (int)ceilf(seconds / in->dt);
    for (int i = 0; i < steps; i++) n = viewmodel_build(vm, in, out, VIEWMODEL_MAX);
    return n;
}

static float dist3(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static quat inst_q(const entity_instance *e) { return (quat){e->rot[0], e->rot[1], e->rot[2], e->rot[3]}; }

/* Finite, unit quaternions, positive sizes, every corner in front of the
 * near plane (unless near is 0: a rifle's stock is behind the eye, under
 * the cheek, and the near plane cuts it). */
static int vm_sane_near(const entity_instance *e, int n, int near)
{
    for (int i = 0; i < n; i++) {
        const float *f = &e[i].pos[0];
        for (int k = 0; k < 3; k++)
            if (!isfinite(f[k]) || !isfinite(e[i].scale[k]) || !(e[i].scale[k] > 0.0f)) return 0;
        float l = 0.0f;
        for (int k = 0; k < 4; k++) {
            if (!isfinite(e[i].rot[k])) return 0;
            l += e[i].rot[k] * e[i].rot[k];
        }
        if (fabsf(sqrtf(l) - 1.0f) > 1e-4f || !isfinite(e[i].light)) return 0;
        for (int c = 0; c < 8; c++) {
            float v[3] = {(c & 1 ? 0.5f : -0.5f) * e[i].scale[0], (c & 2 ? 0.5f : -0.5f) * e[i].scale[1],
                          (c & 4 ? 0.5f : -0.5f) * e[i].scale[2]};
            float r[3] = {0};
            quat_apply(inst_q(&e[i]), v, r);
            if (near && !(e[i].pos[2] + r[2] < NEAR_Z)) return 0;
        }
    }
    return 1;
}

static int vm_sane(const entity_instance *e, int n) { return vm_sane_near(e, n, 1); }

/* A centre inside a 70 degree vertical view on a 4:3 or wider screen. */
static int in_view(const float p[3])
{
    float d = -p[2];
    return d > 0.05f && fabsf(p[1]) < TAN_HALF_VFOV * d && fabsf(p[0]) < TAN_HALF_VFOV * (4.0f / 3.0f) * d;
}

/* How many of a cube's faces point toward the eye at the origin. */
static int faces_seen(const entity_instance *e)
{
    int seen = 0;
    for (int f = 0; f < 6; f++) {
        float nrm[3] = {0, 0, 0}, r[3] = {0};
        nrm[f / 2] = (f & 1) ? -1.0f : 1.0f;
        quat_apply(inst_q(e), nrm, r);
        float to_eye = -(r[0] * e->pos[0] + r[1] * e->pos[1] + r[2] * e->pos[2]);
        seen += to_eye > 0.0f;
    }
    return seen;
}

/* Angle between two orientations, rad. */
static float quat_angle(quat a, quat b)
{
    float d = fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0f * acosf(d > 1.0f ? 1.0f : d);
}

static uint32_t xs(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static float frnd(uint32_t *s) { return (float)(xs(s) >> 8) * (1.0f / 16777216.0f); }

/* ---- what is drawn */

static void test_vm_parts(void)
{
    entity_instance out[VIEWMODEL_MAX] = {0};
    viewmodel vm = {0};
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(0);
    int n = vm_run(&vm, &in, 0.5f, out);
    CHECK(n == VIEWMODEL_ARM_PARTS); /* the bare arm: forearm, wrist, palm, fingers, thumb */
    for (int i = 0; i < n; i++) CHECK(out[i].tex == (uint32_t)T_SKIN * 0x010101u);
    CHECK(vm_sane(out, n) && in_view(out[HAND].pos));
    /* The forearm tapers to a wrist and the hand sits at its far end,
     * about 0.55 m along the arm, further from the eye. */
    const entity_instance *fore = &out[0], *wrist = &out[1], *palm = &out[HAND], *fingers = &out[HAND + 1],
                          *thumb = &out[HAND + 2];
    /* Drawn 1.3 times the figure's 8.5 cm, so the long arm doesn't look spindly. */
    CHECK(fabsf(fore->scale[2] + wrist->scale[2] - 0.5f) < 0.03f && fabsf(fore->scale[0] - 1.3f * 0.085f) < 1e-5f);
    CHECK(wrist->scale[0] < fore->scale[0] - 0.01f && wrist->scale[1] < fore->scale[1] - 0.01f);
    CHECK(-wrist->pos[2] > -fore->pos[2] && -palm->pos[2] > -wrist->pos[2]);
    CHECK(-palm->pos[2] > -fore->pos[2] + 0.2f);
    /* A flat palm, wider than the wrist and less than half as thick as wide. */
    CHECK(palm->scale[0] > wrist->scale[0] && palm->scale[0] < 0.13f && palm->scale[1] < 0.5f * palm->scale[0]);
    /* The fingers curl down under its front; the thumb sits on the inner
     * side, toward the crosshair, and is the slimmest part. */
    CHECK(-fingers->pos[2] > -palm->pos[2] && fingers->pos[1] < palm->pos[1]);
    CHECK(thumb->pos[0] < palm->pos[0] - 0.02f && thumb->scale[0] < 0.04f && thumb->scale[1] < 0.04f);
    CHECK(palm->pos[0] > 0.1f && palm->pos[1] < -0.1f); /* lower right */

    in.held_id = B_LOG;
    n = vm_run(&vm, &in, 0.5f, out);
    CHECK(n == HELD + 1 && vm.shown_id == B_LOG);
    const block_def *d = block_get(B_LOG);
    CHECK(out[HELD].tex == ((uint32_t)d->tex[0] | (uint32_t)d->tex[2] << 8 | (uint32_t)d->tex[3] << 16));
    CHECK(fabsf(out[HELD].scale[0] - 0.18f) < 1e-6f && fabsf(out[HELD].scale[2] - 0.18f) < 1e-6f);
    CHECK(faces_seen(&out[HELD]) == 3); /* top and two sides */
    CHECK(vm_sane(out, n) && in_view(out[HELD].pos));
    CHECK(dist3(out[HELD].pos, out[HAND].pos) < 0.2f); /* in the hand */

    in.held_id = I_APPLE;
    n = vm_run(&vm, &in, 0.5f, out);
    item_model apple = {0};
    CHECK(itemmodel_build(I_APPLE, &apple));
    CHECK(n == HELD + apple.n && vm.shown_id == I_APPLE);
    /* The apple's own boxes, tinted, sized to sit in the palm, by the hand. */
    float lo[3] = {9.0f, 9.0f, 9.0f}, hi[3] = {-9.0f, -9.0f, -9.0f};
    for (int i = 0; i < apple.n; i++) {
        CHECK(out[HELD + i].tex == (uint32_t)apple.box[i].layer * 0x010101u);
        CHECK(out[HELD + i].tint == entity_tint(apple.box[i].rgb));
        CHECK(in_view(out[HELD + i].pos) && dist3(out[HELD + i].pos, out[HAND].pos) < 0.25f);
        for (int a = 0; a < 3; a++) {
            lo[a] = fminf(lo[a], out[HELD + i].pos[a]);
            hi[a] = fmaxf(hi[a], out[HELD + i].pos[a]);
        }
    }
    float span = fmaxf(hi[0] - lo[0], fmaxf(hi[1] - lo[1], hi[2] - lo[2]));
    CHECK(span > 0.02f && span < 0.2f);
    CHECK(vm_sane(out, n));
    /* A knife is held by its handle: its blade runs on from the fist. */
    in.held_id = item_tool_id(TOOL_KNIFE, TIER_IRON);
    n = vm_run(&vm, &in, 0.5f, out);
    item_model knife = {0};
    CHECK(itemmodel_build(in.held_id, &knife));
    CHECK(n == HELD + knife.n && vm_sane(out, n));
    float reach = 0.0f;
    for (int i = HELD; i < n; i++) reach = fmaxf(reach, dist3(out[i].pos, out[HAND].pos));
    CHECK(reach > 0.15f && reach < 0.4f);
    CHECK(dist3(out[HELD].pos, out[HAND].pos) < 0.1f); /* the handle in the hand */

    /* Brightness passes through; 0 means "not set" (full brightness). */
    in.light = 0.3f;
    n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    CHECK(fabsf(out[0].light - 0.3f) < 1e-6f && fabsf(out[n - 1].light - 0.3f) < 1e-6f);
    in.light = 0.0f;
    viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    CHECK(fabsf(out[0].light - 1.0f) < 1e-6f);

    /* A short buffer gets the first parts only; NULL gets none. */
    in.light = 1.0f;
    CHECK(viewmodel_build(&vm, &in, out, 1) == 1);
    CHECK(viewmodel_build(&vm, &in, NULL, 0) == 0);
}

/* ---- equip */

static void test_vm_equip(void)
{
    entity_instance out[VIEWMODEL_MAX] = {0};
    viewmodel vm = {0};
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(B_STONE);
    vm_run(&vm, &in, 1.0f, out);
    CHECK(vm.equip == 1.0f && vm.shown_id == B_STONE);
    float rest[3] = {0};
    memcpy(rest, out[HAND].pos, sizeof rest);
    /* A twin that keeps the stone, stepped in lockstep: the arm breathes,
     * so "back up" means back where the twin is. */
    viewmodel twin = vm;
    viewmodel_input tin = in;
    entity_instance tout[VIEWMODEL_MAX] = {0};

    /* Switch: the stone stays drawn while the arm drops, the planks appear
     * at the bottom, and the arm is back within 0.5 s. */
    in.held_id = B_PLANKS;
    float lowest = rest[1], swapped_at = -1.0f, up_at = -1.0f;
    int ok = 1;
    for (int k = 1; k <= 36; k++) {
        float t = (float)k * VM_DT; /* elapsed once this frame is built */
        int n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        viewmodel_build(&twin, &tin, tout, VIEWMODEL_MAX);
        ok &= vm_sane(out, n) && n == HELD + 1;
        if (out[HAND].pos[1] < lowest) lowest = out[HAND].pos[1];
        if (swapped_at < 0.0f && vm.shown_id == B_PLANKS) {
            swapped_at = t;
            /* Swapped out of sight: hand and card are below the bottom edge. */
            ok &= !in_view(out[HAND].pos) && !in_view(out[HELD].pos);
        }
        if (swapped_at >= 0.0f && up_at < 0.0f && vm.equip >= 1.0f) up_at = t;
        if (t < 0.05f) ok &= vm.shown_id == B_STONE;
    }
    CHECK(ok);
    CHECK(lowest < rest[1] - 0.3f);
    CHECK(swapped_at > 0.08f && swapped_at < 0.2f);
    CHECK(up_at > 0.2f && up_at < 0.5f);
    CHECK(dist3(out[HAND].pos, tout[HAND].pos) < 1e-4f);
    const block_def *d = block_get(B_PLANKS);
    CHECK((out[HELD].tex & 255u) == d->tex[0]);

    /* Back to the bare hand: the arm alone once swapped. */
    in.held_id = 0;
    CHECK(vm_run(&vm, &in, 0.5f, out) == VIEWMODEL_ARM_PARTS && vm.equip == 1.0f);
}

/* ---- swing */

static void test_vm_swing(void)
{
    entity_instance out[VIEWMODEL_MAX] = {0};
    viewmodel vm = {0};
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(B_DIRT);
    vm_run(&vm, &in, 1.0f, out);
    float rest[3] = {0};
    memcpy(rest, out[HAND].pos, sizeof rest);
    viewmodel twin = vm; /* never swings: where the arm rests at each moment */
    viewmodel_input tin = in;
    entity_instance tout[VIEWMODEL_MAX] = {0};

    /* One action: interact sets swing to 1 and eases it out at 3.5 /s. */
    in.swing = 1.0f;
    float peak = 0.0f, peak_t = 0.0f, peak_dx = 0.0f, peak_dy = 0.0f;
    int ok = 1;
    for (int k = 1; k <= 30; k++) {
        float t = (float)k * VM_DT;
        int n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        viewmodel_build(&twin, &tin, tout, VIEWMODEL_MAX);
        ok &= vm_sane(out, n) && in_view(out[HAND].pos) && in_view(out[HELD].pos);
        float d = dist3(out[HAND].pos, tout[HAND].pos);
        if (d > peak) {
            peak = d;
            peak_t = t;
            peak_dx = out[HAND].pos[0] - tout[HAND].pos[0];
            peak_dy = out[HAND].pos[1] - tout[HAND].pos[1];
        }
        in.swing = fmaxf(0.0f, in.swing - 3.5f * VM_DT);
    }
    CHECK(ok);
    CHECK(peak > 0.15f);                      /* a real sweep, not a twitch */
    CHECK(peak_dx < -0.1f && peak_dy < -0.05f); /* down and in toward the crosshair */
    CHECK(peak_t > 0.05f && peak_t < 0.15f);  /* quick down stroke */
    CHECK(dist3(out[HAND].pos, tout[HAND].pos) < 1e-4f && dist3(out[HELD].pos, tout[HELD].pos) < 1e-4f); /* home */

    /* Holding the button on a block: interact re-arms swing every 0.25 s
     * (a chip); the arm keeps swinging and never jumps. At 1 kHz the hand
     * moves under 1 cm per step (its top speed is ~5 m/s); a restart from
     * rest mid-return would jump 10 cm. */
    viewmodel_input h = in;
    h.dt = 0.001f;
    h.swinging = 1;
    h.swing = 1.0f;
    float prev[3] = {0}, max_step = 0.0f, late_max = 0.0f, chip = 0.0f;
    viewmodel_build(&vm, &h, out, VIEWMODEL_MAX);
    memcpy(prev, out[HAND].pos, sizeof prev);
    for (int i = 1; i < 2000; i++) {
        chip += h.dt;
        h.swing = fmaxf(0.0f, h.swing - 3.5f * h.dt);
        if (chip >= 0.25f) {
            chip = 0.0f;
            h.swing = 1.0f;
        }
        viewmodel_build(&vm, &h, out, VIEWMODEL_MAX);
        float step = dist3(out[HAND].pos, prev);
        if (step > max_step) max_step = step;
        if (i > 1500 && dist3(out[HAND].pos, rest) > late_max) late_max = dist3(out[HAND].pos, rest);
        memcpy(prev, out[HAND].pos, sizeof prev);
    }
    CHECK(max_step < 0.01f);
    CHECK(late_max > 0.1f); /* still swinging after 1.5 s */
}

/* ---- movement */

static void test_vm_motion(void)
{
    entity_instance out[VIEWMODEL_MAX] = {0}, rest_out[VIEWMODEL_MAX] = {0};
    viewmodel vm = {0};
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(B_STONE);
    vm_run(&vm, &in, 1.0f, rest_out);

    /* Sprint: the arm drops and tilts. */
    in.sprint = 1.0f;
    viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    CHECK(out[HAND].pos[1] < rest_out[HAND].pos[1] - 0.08f);
    CHECK(quat_angle(inst_q(&out[HAND]), inst_q(&rest_out[HAND])) > 0.3f);
    CHECK(vm_sane(out, 3) && in_view(out[HAND].pos) && in_view(out[HELD].pos));
    in.sprint = 0.0f;

    /* Walking: a figure-eight. Half a stride cycle later (the other foot)
     * the sideways offset is mirrored and the height repeats; across the
     * cycle the hand dips twice for one sway. Breathing is off while the
     * bob is full, so the offsets are the bob's alone. */
    in.bob = 1.0f;
    float x0 = rest_out[HAND].pos[0];
    in.stride = 0.25f * (float)MC_PI;
    viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    float xa = out[HAND].pos[0] - x0, ya = out[HAND].pos[1];
    in.stride = 1.25f * (float)MC_PI;
    viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    float xb = out[HAND].pos[0] - x0, yb = out[HAND].pos[1];
    CHECK(xa > 0.015f && fabsf(xa + xb) < 0.004f);
    CHECK(fabsf(ya - yb) < 1e-3f);
    int dips = 0;
    float yprev = 0.0f, dyprev = 0.0f;
    for (int i = 0; i <= 64; i++) {
        in.stride = 2.0f * (float)MC_PI * (float)i / 64.0f;
        viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        float dy = out[HAND].pos[1] - yprev;
        if (i > 1 && dyprev < 0.0f && dy >= 0.0f) dips++;
        dyprev = dy;
        yprev = out[HAND].pos[1];
    }
    CHECK(dips == 2);
    in.bob = 0.0f;
    in.stride = 0.0f;

    /* Standing still the arm breathes: a few mm at 15 breaths a minute. */
    float ymin = 1.0f, ymax = -1.0f;
    for (int k = 0; k < 240; k++) { /* one breath, 4 s */
        viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        ymin = fminf(ymin, out[HAND].pos[1]);
        ymax = fmaxf(ymax, out[HAND].pos[1]);
    }
    CHECK(ymax - ymin > 0.004f && ymax - ymin < 0.02f);

    /* Mouse lag: turning right at 3 rad/s swings the arm left, by at most
     * the 0.12 rad clamp; it comes back once the look stops. */
    vm_run(&vm, &in, 0.5f, rest_out);
    in.look_dx = 3.0f * VM_DT;
    vm_run(&vm, &in, 0.3f, out);
    float shift = out[HAND].pos[0] - rest_out[HAND].pos[0];
    CHECK(shift < -0.05f && shift > -0.12f); /* 0.12 rad at under 1 m */
    CHECK(vm.lag_x < 0.0f && vm.lag_x >= -0.12f);
    in.look_dx = 0.0f;
    in.look_dy = 3.0f * VM_DT; /* looking up: the arm sinks */
    vm_run(&vm, &in, 0.3f, out);
    CHECK(out[HAND].pos[1] < rest_out[HAND].pos[1] - 0.05f && fabsf(vm.lag_x) < 0.01f);
    in.look_dy = 0.0f;
    vm_run(&vm, &in, 0.8f, out);
    CHECK(fabsf(vm.lag_x) < 1e-3f && fabsf(vm.lag_y) < 1e-3f);

    /* Eating: the apple comes up to the mouth (just below the eye) and
     * bobs with the chewing. */
    in.held_id = I_APPLE;
    vm_run(&vm, &in, 0.6f, rest_out);
    const float mouth[3] = {0.0f, -0.1f, 0.0f};
    in.eating = 0.5f;
    ymin = 1.0f;
    ymax = -1.0f;
    item_model apple = {0};
    itemmodel_build(I_APPLE, &apple);
    int n3 = 1;
    for (int k = 0; k < 60; k++) {
        n3 &= viewmodel_build(&vm, &in, out, VIEWMODEL_MAX) == HELD + apple.n;
        ymin = fminf(ymin, out[HELD].pos[1]);
        ymax = fmaxf(ymax, out[HELD].pos[1]);
    }
    CHECK(n3);
    CHECK(dist3(out[HELD].pos, mouth) < dist3(rest_out[HELD].pos, mouth) - 0.25f);
    CHECK(ymax - ymin > 0.01f);
    CHECK(vm_sane(out, HELD + apple.n) && in_view(out[HELD].pos));
    in.eating = 0.99f; /* on its way back down */
    viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    CHECK(dist3(out[HELD].pos, rest_out[HELD].pos) < 0.05f);
}

/* ---- robustness */

static void test_vm_robust(void)
{
    entity_instance out[VIEWMODEL_MAX] = {0}, again[VIEWMODEL_MAX] = {0};
    viewmodel vm = {0};

    /* dt = 0: finite output, nothing advances, the same pose twice. */
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(B_GLASS);
    in.dt = 0.0f;
    in.look_dx = 0.1f;
    in.swing = 1.0f;
    int n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    viewmodel_build(&vm, &in, again, VIEWMODEL_MAX);
    CHECK(n == HELD + 1 && vm_sane(out, n));
    CHECK(vm.time == 0.0f && vm.lag_x == 0.0f && vm.equip == 0.0f);
    CHECK(memcmp(out, again, (size_t)n * sizeof out[0]) == 0);

    /* Garbage in (NaN, huge dt, ids no item has) never leaks out. */
    in = vm_input(0xFFFFFFFDu);
    in.dt = NAN;
    in.look_dx = INFINITY;
    in.stride = NAN;
    in.sprint = 7.0f;
    n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    CHECK(n == VIEWMODEL_ARM_PARTS && vm_sane(out, n));
    in.dt = 30.0f;
    n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    CHECK(vm_sane(out, n) && isfinite(vm.time));

    /* Random play: every frame is sane, and once raised (and not eating)
     * the hand and the held thing stay on screen through any mix of
     * walking, sprinting, swinging and fast looks. */
    viewmodel_init(&vm);
    uint32_t s = 0x9e3779b9u;
    in = vm_input(0);
    int ok = 1, seen = 0, frames_up = 0;
    for (int i = 0; i < 6000; i++) {
        if (xs(&s) % 90u == 0u) {
            static const item_id IDS[] = {0,
                                          B_STONE,
                                          B_GLASS,
                                          B_CAMPFIRE,
                                          I_STICK,
                                          I_APPLE,
                                          I_WATER_BUCKET,
                                          99,
                                          I_RIFLE,
                                          I_TOOL_FIRST + TOOL_AXE * TIER_COUNT + TIER_STONE,
                                          I_TOOL_FIRST + TOOL_SHOVEL * TIER_COUNT + TIER_IRON};
            in.held_id = IDS[xs(&s) % 11u];
        }
        in.dt = 0.004f + 0.04f * frnd(&s);
        in.stride = fmodf(in.stride + 6.0f * in.dt, 2.0f * (float)MC_PI);
        if (xs(&s) % 120u == 0u) in.bob = frnd(&s);
        if (xs(&s) % 120u == 0u) in.sprint = frnd(&s) < 0.5f ? 0.0f : 1.0f;
        in.swinging = (i / 200) % 3 == 1;
        in.swing = (xs(&s) % 20u == 0u) ? 1.0f : fmaxf(0.0f, in.swing - 3.5f * in.dt);
        in.look_dx = (frnd(&s) - 0.5f) * 20.0f * in.dt;
        in.look_dy = (frnd(&s) - 0.5f) * 20.0f * in.dt;
        in.eating = (i / 500) % 4 == 3 ? fmodf((float)(i % 500) / 200.0f, 1.0f) : 0.0f;
        if (xs(&s) % 60u == 0u) in.aim = frnd(&s) < 0.5f ? 0.0f : frnd(&s);
        n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        ok &= n >= 2 && n <= VIEWMODEL_MAX && vm_sane_near(out, n, vm.shown_id != I_RIFLE);
        if (vm.equip >= 1.0f && in.eating == 0.0f) {
            frames_up++;
            for (int k = 1; k < n; k++) seen += in_view(out[k].pos);
            int item = 2 * viewmodel_hands(vm.shown_id), shown = 0; /* the first of the item's parts */
            for (int k = item; k < n; k++) shown |= in_view(out[k].pos);
            ok &= (item == 4 || in_view(out[HAND].pos)) && (n <= item || shown);
        }
    }
    CHECK(ok);
    CHECK(frames_up > 2000 && seen >= frames_up);
}

/* Distance from p to the line through a box's centre along its own y,
 * the long axis of a haft or a rifle. */
static float off_axis(const float p[3], const entity_instance *box)
{
    const float up[3] = {0.0f, 1.0f, 0.0f};
    float a[3] = {0}, d[3] = {p[0] - box->pos[0], p[1] - box->pos[1], p[2] - box->pos[2]};
    quat_apply(inst_q(box), up, a);
    float t = d[0] * a[0] + d[1] * a[1] + d[2] * a[2];
    for (int k = 0; k < 3; k++) d[k] -= a[k] * t;
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

#define R_HAND VIEWMODEL_PALM /* two hands: the right arm's parts, the left's, the item */
#define L_HAND (VIEWMODEL_ARM_PARTS + VIEWMODEL_PALM)
#define ITEM (2 * VIEWMODEL_ARM_PARTS)

static void test_vm_two_hands(void)
{
    entity_instance out[VIEWMODEL_MAX] = {{0}};
    CHECK(viewmodel_hands(item_tool_id(TOOL_AXE, TIER_IRON)) == 2 && viewmodel_hands(I_RIFLE) == 2);
    CHECK(viewmodel_hands(item_tool_id(TOOL_SHOVEL, TIER_STONE)) == 2 && viewmodel_hands(0) == 1);
    CHECK(viewmodel_hands(item_tool_id(TOOL_KNIFE, TIER_IRON)) == 1 && viewmodel_hands(B_STONE) == 1);

    /* An axe at rest: both fists close round the haft (box 0 of the
     * model), the left lower down it than the right, the head in view and
     * well past the hands. The forearms run from the fists back out of the
     * bottom of the view, as far as the arms reach. */
    viewmodel vm;
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(item_tool_id(TOOL_AXE, TIER_IRON));
    item_model axe = {0};
    CHECK(itemmodel_build(in.held_id, &axe));
    int n = vm_run(&vm, &in, 0.5f, out);
    CHECK(n == ITEM + axe.n && vm_sane(out, n));
    CHECK(off_axis(out[R_HAND].pos, &out[ITEM]) < 0.02f && off_axis(out[L_HAND].pos, &out[ITEM]) < 0.02f);
    CHECK(out[L_HAND].pos[1] < out[R_HAND].pos[1] - 0.1f);
    CHECK(in_view(out[R_HAND].pos) && in_view(out[ITEM + 1].pos));
    CHECK(dist3(out[ITEM + 1].pos, out[R_HAND].pos) > 0.3f);
    for (int s = 0; s < 2; s++) {
        const float *f = out[2 * s].pos, *h = out[2 * s + 1].pos;
        CHECK(f[1] < h[1] && f[2] > h[2] && dist3(f, h) < 0.25f);
    }
    const float rest_head[3] = {out[ITEM + 1].pos[0], out[ITEM + 1].pos[1], out[ITEM + 1].pos[2]};
    const float rest_r[3] = {out[R_HAND].pos[0], out[R_HAND].pos[1], out[R_HAND].pos[2]};

    /* One hit: the head goes up past the hands (wind-up), comes down below
     * where it was carried, the right fist slides down to the left one, and
     * after 0.8 s it's back where it started. The edge (box 3, the model's
     * +x) is lower than the poll (box 4) when it lands, so it leads. */
    viewmodel twin = vm; /* the same, never hit: breathing moves both alike */
    in.swing = 1.0f;
    viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    in.swing = 0.0f;
    entity_instance tout[VIEWMODEL_MAX] = {{0}};
    viewmodel_build(&twin, &in, tout, VIEWMODEL_MAX);
    float top = -9.0f, low = 9.0f, closest = 9.0f;
    int edge_leads = 0, sane = 1;
    for (int i = 0; i < 48; i++) {
        n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        sane &= vm_sane(out, n) && n == ITEM + axe.n;
        top = fmaxf(top, out[ITEM + 1].pos[1]);
        if (out[ITEM + 1].pos[1] < low) {
            low = out[ITEM + 1].pos[1];
            edge_leads = out[ITEM + 3].pos[1] < out[ITEM + 4].pos[1];
        }
        closest = fminf(closest, dist3(out[R_HAND].pos, out[L_HAND].pos));
        sane &= off_axis(out[R_HAND].pos, &out[ITEM]) < 0.02f && off_axis(out[L_HAND].pos, &out[ITEM]) < 0.02f;
    }
    CHECK(sane && edge_leads);
    CHECK(top > rest_head[1] + 0.1f && low < rest_head[1] - 0.1f);
    CHECK(closest < 0.15f);
    vm_run(&twin, &in, 48.0f * VM_DT, tout);
    vm_run(&vm, &in, 0.2f, out);
    vm_run(&twin, &in, 0.2f, tout);
    CHECK(dist3(out[ITEM + 1].pos, tout[ITEM + 1].pos) < 1e-4f && dist3(out[R_HAND].pos, tout[R_HAND].pos) < 1e-4f);
    CHECK(dist3(out[R_HAND].pos, rest_r) < 0.01f);

    /* Holding the button on a block chops again and again, never jumping
     * more than a few centimetres a frame. */
    in.swinging = 1;
    float jump = 0.0f, prev[3] = {rest_head[0], rest_head[1], rest_head[2]}, prev_t = vm.swing_t;
    float lowest = 9.0f;
    int chops = 0;
    for (int i = 0; i < 180; i++) {
        viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        if (vm.swing_t < prev_t) {
            chops++;
            jump = fmaxf(jump, dist3(out[ITEM + 1].pos, prev));
        }
        prev_t = vm.swing_t;
        memcpy(prev, out[ITEM + 1].pos, sizeof prev);
        if (i > 100) lowest = fminf(lowest, out[ITEM + 1].pos[1]);
    }
    CHECK(chops >= 3 && jump < 0.03f && lowest < rest_head[1] - 0.1f);
    in.swinging = 0;

    /* A shovel: both fists on the shaft (box 3 of the model, after the D
     * grip), its blade facing up rather than edge on. */
    viewmodel_init(&vm);
    in = vm_input(item_tool_id(TOOL_SHOVEL, TIER_IRON));
    item_model shovel = {0};
    CHECK(itemmodel_build(in.held_id, &shovel));
    n = vm_run(&vm, &in, 0.5f, out);
    CHECK(n == ITEM + shovel.n && vm_sane(out, n));
    CHECK(off_axis(out[R_HAND].pos, &out[ITEM + 3]) < 0.03f);
    const float blade_normal[3] = {0.0f, 0.0f, 1.0f};
    float bn[3] = {0};
    quat_apply(inst_q(&out[ITEM + 5]), blade_normal, bn);
    CHECK(fabsf(bn[1]) > 0.5f);
}

static void test_vm_rifle(void)
{
    entity_instance out[VIEWMODEL_MAX] = {{0}};
    item_model rifle = {0};
    CHECK(itemmodel_build(I_RIFLE, &rifle));
    /* The model's boxes: 1 the wrist of the stock, 2 the fore-end, 4 the
     * barrel, 10 the front blade, 11 and 12 the rear sight's ears, 13 and
     * 14 the sights' bases. */
    viewmodel vm;
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(I_RIFLE);
    int n = vm_run(&vm, &in, 0.5f, out);
    CHECK(n == ITEM + rifle.n && vm_sane_near(out, n, 0));
    /* At the hip: the right fist on the wrist, the left under the fore-end,
     * the muzzle ahead, the sights well off the line of sight. */
    CHECK(off_axis(out[R_HAND].pos, &out[ITEM + 1]) < 0.07f && off_axis(out[L_HAND].pos, &out[ITEM + 2]) < 0.07f);
    CHECK(out[ITEM + 10].pos[2] < out[ITEM + 1].pos[2] - 0.5f);
    float hip_off = hypotf(out[ITEM + 10].pos[0], out[ITEM + 10].pos[1]);
    CHECK(hip_off > 0.1f);

    /* Aimed: the front post sits on the line of sight, far out and just
     * under it, the rear posts either side of the line, and the hands stay
     * on the rifle. */
    in.aim = 1.0f;
    n = vm_run(&vm, &in, 1.0f, out);
    CHECK(n == ITEM + rifle.n && vm_sane_near(out, n, 0));
    const float *post = out[ITEM + 10].pos;
    float tip = post[1] + 0.006f; /* the blade is 12 mm tall, standing up */
    CHECK(post[2] < -0.6f && fabsf(post[0] / post[2]) < 0.004f && fabsf(tip / post[2]) < 0.005f);
    CHECK(post[1] < 0.0f && tip < 0.0f);
    /* The eye rides high over the stock, so the fist on its wrist is well
     * under the line of sight (more than 48 degrees down), not across it. */
    CHECK(-out[R_HAND].pos[1] > 1.1f * -out[R_HAND].pos[2]);
    CHECK(out[ITEM + 11].pos[0] * out[ITEM + 12].pos[0] < 0.0f);
    CHECK(fabsf(out[ITEM + 11].pos[0] + out[ITEM + 12].pos[0]) < 0.003f);
    CHECK(off_axis(out[R_HAND].pos, &out[ITEM + 1]) < 0.07f && off_axis(out[L_HAND].pos, &out[ITEM + 2]) < 0.07f);
    /* Halfway up, it's between the two. */
    in.aim = 0.5f;
    vm_run(&vm, &in, 0.1f, out);
    float mid_off = hypotf(out[ITEM + 10].pos[0], out[ITEM + 10].pos[1]);
    CHECK(mid_off > 0.01f && mid_off < hip_off);

    /* A shot: the muzzle climbs within a tenth of a second and settles
     * back on the line within half a second. */
    in.aim = 1.0f;
    vm_run(&vm, &in, 1.0f, out);
    const float aimed[3] = {post[0], post[1], post[2]};
    viewmodel still = vm;
    in.swing = 1.0f;
    viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    in.swing = 0.0f;
    float climb = 0.0f;
    for (int i = 0; i < 6; i++) {
        viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        climb = fmaxf(climb, out[ITEM + 10].pos[1] - aimed[1]);
    }
    CHECK(climb > 0.03f);
    vm_run(&vm, &in, 0.5f, out);
    entity_instance sout[VIEWMODEL_MAX] = {{0}};
    vm_run(&still, &in, 0.5f + 7.0f * VM_DT, sout);
    CHECK(dist3(out[ITEM + 10].pos, sout[ITEM + 10].pos) < 1e-4f);

    /* Garbage aim is treated as none. */
    in.aim = NAN;
    n = vm_run(&vm, &in, 0.5f, out);
    CHECK(vm_sane_near(out, n, 0) && hypotf(out[ITEM + 10].pos[0], out[ITEM + 10].pos[1]) > 0.1f);
    /* Aiming means nothing with any other item in hand. */
    viewmodel_init(&vm);
    in = vm_input(item_tool_id(TOOL_AXE, TIER_IRON));
    vm_run(&vm, &in, 0.5f, out);
    viewmodel unaimed = vm;
    entity_instance uout[VIEWMODEL_MAX] = {{0}};
    vm_run(&unaimed, &in, 0.5f, uout);
    in.aim = 1.0f;
    vm_run(&vm, &in, 0.5f, out);
    CHECK(dist3(out[ITEM + 1].pos, uout[ITEM + 1].pos) < 1e-4f);
}

/* Raising the rifle is a mount: it swings through an arc below the
 * straight line from the hip to the cheek, overshoots a little and settles
 * within half a second, never jumping; once aimed it drifts, but by less
 * than a few milliradians. */
static void test_vm_mount(void)
{
    entity_instance out[VIEWMODEL_MAX] = {{0}};
    viewmodel vm;
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(I_RIFLE);
    vm_run(&vm, &in, 1.0f, out);
    viewmodel up = vm;
    entity_instance aimed[VIEWMODEL_MAX] = {{0}};
    in.aim = 1.0f;
    vm_run(&up, &in, 1.0f, aimed);
    const float hip[3] = {out[ITEM + 10].pos[0], out[ITEM + 10].pos[1], out[ITEM + 10].pos[2]};
    const float *end = aimed[ITEM + 10].pos;
    float line[3] = {end[0] - hip[0], end[1] - hip[1], end[2] - hip[2]};
    float len = sqrtf(line[0] * line[0] + line[1] * line[1] + line[2] * line[2]);
    for (int k = 0; k < 3; k++) line[k] /= len;
    float below = 0.0f, over = 0.0f, jump = 0.0f, prev[3] = {hip[0], hip[1], hip[2]};
    for (int i = 0; i < 60; i++) {
        viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        const float *p = out[ITEM + 10].pos;
        const float d[3] = {p[0] - hip[0], p[1] - hip[1], p[2] - hip[2]};
        float along = d[0] * line[0] + d[1] * line[1] + d[2] * line[2];
        if (along > 0.2f * len && along < 0.8f * len) below = fmaxf(below, hip[1] + line[1] * along - p[1]);
        over = fmaxf(over, vm.aim_x);
        jump = fmaxf(jump, dist3(p, prev));
        memcpy(prev, p, sizeof prev);
    }
    CHECK(below > 0.02f);
    CHECK(over > 1.03f && over < 1.2f);
    CHECK(jump < 0.06f);
    CHECK(fabsf(vm.aim_x - 1.0f) < 0.02f);

    /* Held: the post wanders, but stays within 4 mrad of the line. */
    float lo = 9.0f, hi = -9.0f, worst = 0.0f;
    for (int i = 0; i < 600; i++) {
        viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        const float *p = out[ITEM + 10].pos;
        float a = p[0] / -p[2];
        lo = fminf(lo, a);
        hi = fmaxf(hi, a);
        worst = fmaxf(worst, fabsf(a));
    }
    CHECK(hi - lo > 0.001f && worst < 0.004f);

    /* Garbage in the spring starts it clean, and another item drops it. */
    vm.aim_v = NAN;
    int n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    CHECK(vm_sane_near(out, n, 0) && isfinite(vm.aim_x));
    in.held_id = 0;
    vm_run(&vm, &in, 1.0f, out);
    CHECK(vm.aim_x == 0.0f && vm.aim_v == 0.0f);
}

void test_visual_all(void)
{
    test_vm_parts();
    test_vm_equip();
    test_vm_swing();
    test_vm_motion();
    test_vm_robust();
    test_vm_two_hands();
    test_vm_rifle();
    test_vm_mount();
}
