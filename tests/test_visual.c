/* The view model and the other animation state (viewmodel.c). */
#include "item.h"
#include "test_util.h"
#include "viewmodel.h"

#include <math.h>
#include <string.h>

#define VM_DT (1.0f / 60.0f)
#define TAN_HALF_VFOV 0.7002f /* tan(35 deg): the view-model pass draws a 70 degree vertical view */
#define NEAR_Z (-0.02f)       /* its near plane */
#define HAND 1                /* instance order: sleeve, hand, held */
#define HELD 2

static viewmodel_input vm_input(int held)
{
    viewmodel_input in;
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
 * near plane. */
static int vm_sane(const entity_instance *e, int n)
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
            float r[3];
            quat_apply(inst_q(&e[i]), v, r);
            if (!(e[i].pos[2] + r[2] < NEAR_Z)) return 0;
        }
    }
    return 1;
}

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
        float nrm[3] = {0, 0, 0}, r[3];
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
    entity_instance out[VIEWMODEL_MAX];
    viewmodel vm;
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(0);
    int n = vm_run(&vm, &in, 0.5f, out);
    CHECK(n == 2); /* the bare hand: sleeve and hand */
    CHECK(out[0].tex == (uint32_t)T_SLEEVE * 0x010101u && out[HAND].tex == (uint32_t)T_SKIN * 0x010101u);
    CHECK(vm_sane(out, n) && in_view(out[HAND].pos));
    /* The sleeve runs 0.5 m along the arm and the hand sits at its far end,
     * further from the eye. */
    CHECK(fabsf(out[0].scale[2] - 0.5f) < 1e-6f && fabsf(out[0].scale[0] - 0.12f) < 1e-6f);
    CHECK(-out[HAND].pos[2] > -out[0].pos[2] + 0.2f);
    CHECK(out[HAND].pos[0] > 0.1f && out[HAND].pos[1] < -0.1f); /* lower right */

    in.held_id = B_LOG;
    n = vm_run(&vm, &in, 0.5f, out);
    CHECK(n == 3 && vm.shown_id == B_LOG);
    const block_def *d = block_get(B_LOG);
    CHECK(out[HELD].tex == ((uint32_t)d->tex[0] | (uint32_t)d->tex[2] << 8 | (uint32_t)d->tex[3] << 16));
    CHECK(fabsf(out[HELD].scale[0] - 0.22f) < 1e-6f && fabsf(out[HELD].scale[2] - 0.22f) < 1e-6f);
    CHECK(faces_seen(&out[HELD]) == 3); /* top and two sides */
    CHECK(vm_sane(out, n) && in_view(out[HELD].pos));
    CHECK(dist3(out[HELD].pos, out[HAND].pos) < 0.2f); /* in the hand */

    in.held_id = I_APPLE;
    n = vm_run(&vm, &in, 0.5f, out);
    CHECK(n == 3 && vm.shown_id == I_APPLE);
    CHECK(out[HELD].tex == (uint32_t)T_ITEM_APPLE * 0x010101u);
    CHECK(out[HELD].scale[2] < 0.05f && out[HELD].scale[0] > 0.2f); /* a thin card */
    /* The card's face, not its edge, is toward the eye: the angle between
     * its normal and the line of sight is under 60 degrees. */
    float nz[3] = {0.0f, 0.0f, 1.0f}, r[3];
    quat_apply(inst_q(&out[HELD]), nz, r);
    float cosang = fabsf(r[0] * out[HELD].pos[0] + r[1] * out[HELD].pos[1] + r[2] * out[HELD].pos[2]) /
                   sqrtf(out[HELD].pos[0] * out[HELD].pos[0] + out[HELD].pos[1] * out[HELD].pos[1] +
                         out[HELD].pos[2] * out[HELD].pos[2]);
    CHECK(cosang > 0.5f);
    CHECK(vm_sane(out, n) && in_view(out[HELD].pos));

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
    entity_instance out[VIEWMODEL_MAX];
    viewmodel vm;
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(B_STONE);
    vm_run(&vm, &in, 1.0f, out);
    CHECK(vm.equip == 1.0f && vm.shown_id == B_STONE);
    float rest[3];
    memcpy(rest, out[HAND].pos, sizeof rest);
    /* A twin that keeps the stone, stepped in lockstep: the arm breathes,
     * so "back up" means back where the twin is. */
    viewmodel twin = vm;
    viewmodel_input tin = in;
    entity_instance tout[VIEWMODEL_MAX];

    /* Switch: the stone stays drawn while the arm drops, the planks appear
     * at the bottom, and the arm is back within 0.5 s. */
    in.held_id = B_PLANKS;
    float lowest = rest[1], swapped_at = -1.0f, up_at = -1.0f;
    int ok = 1;
    for (int k = 1; k <= 36; k++) {
        float t = (float)k * VM_DT; /* elapsed once this frame is built */
        int n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        viewmodel_build(&twin, &tin, tout, VIEWMODEL_MAX);
        ok &= vm_sane(out, n) && n == 3;
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

    /* Back to the bare hand: two parts once swapped. */
    in.held_id = 0;
    CHECK(vm_run(&vm, &in, 0.5f, out) == 2 && vm.equip == 1.0f);
}

/* ---- swing */

static void test_vm_swing(void)
{
    entity_instance out[VIEWMODEL_MAX];
    viewmodel vm;
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(B_DIRT);
    vm_run(&vm, &in, 1.0f, out);
    float rest[3];
    memcpy(rest, out[HAND].pos, sizeof rest);
    viewmodel twin = vm; /* never swings: where the arm rests at each moment */
    viewmodel_input tin = in;
    entity_instance tout[VIEWMODEL_MAX];

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
    float prev[3], max_step = 0.0f, late_max = 0.0f, chip = 0.0f;
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
    entity_instance out[VIEWMODEL_MAX], rest_out[VIEWMODEL_MAX];
    viewmodel vm;
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
    int n3 = 1;
    for (int k = 0; k < 60; k++) {
        n3 &= viewmodel_build(&vm, &in, out, VIEWMODEL_MAX) == 3;
        ymin = fminf(ymin, out[HELD].pos[1]);
        ymax = fmaxf(ymax, out[HELD].pos[1]);
    }
    CHECK(n3);
    CHECK(dist3(out[HELD].pos, mouth) < dist3(rest_out[HELD].pos, mouth) - 0.25f);
    CHECK(ymax - ymin > 0.01f);
    CHECK(vm_sane(out, 3) && in_view(out[HELD].pos));
    in.eating = 0.99f; /* on its way back down */
    viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    CHECK(dist3(out[HELD].pos, rest_out[HELD].pos) < 0.05f);
}

/* ---- robustness */

static void test_vm_robust(void)
{
    entity_instance out[VIEWMODEL_MAX], again[VIEWMODEL_MAX];
    viewmodel vm;

    /* dt = 0: finite output, nothing advances, the same pose twice. */
    viewmodel_init(&vm);
    viewmodel_input in = vm_input(B_GLASS);
    in.dt = 0.0f;
    in.look_dx = 0.1f;
    in.swing = 1.0f;
    int n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    viewmodel_build(&vm, &in, again, VIEWMODEL_MAX);
    CHECK(n == 3 && vm_sane(out, n));
    CHECK(vm.time == 0.0f && vm.lag_x == 0.0f && vm.equip == 0.0f);
    CHECK(memcmp(out, again, (size_t)n * sizeof out[0]) == 0);

    /* Garbage in (NaN, huge dt, negative ids) never leaks out. */
    in = vm_input(-3);
    in.dt = NAN;
    in.look_dx = INFINITY;
    in.stride = NAN;
    in.sprint = 7.0f;
    n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    CHECK(n == 2 && vm_sane(out, n));
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
            static const int IDS[] = {0, B_STONE, B_GLASS, B_CAMPFIRE, I_STICK, I_APPLE, I_WATER_BUCKET, 99};
            in.held_id = IDS[xs(&s) % 8u];
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
        n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
        ok &= n >= 2 && n <= 3 && vm_sane(out, n);
        if (vm.equip >= 1.0f && in.eating == 0.0f) {
            frames_up++;
            for (int k = 1; k < n; k++) seen += in_view(out[k].pos);
            ok &= in_view(out[HAND].pos) && (n < 3 || in_view(out[HELD].pos));
        }
    }
    CHECK(ok);
    CHECK(frames_up > 2000 && seen >= frames_up);
}

void test_visual_all(void)
{
    test_vm_parts();
    test_vm_equip();
    test_vm_swing();
    test_vm_motion();
    test_vm_robust();
}
