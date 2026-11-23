#include "torch_model.h"
#include "block.h"
#include <math.h>

/* One part in the torch's own frame: y up from the handle's foot. */
typedef struct {
    float pos[3];
    float size[3];
    float turn; /* about y, radians */
    uint32_t tex;
    int glows;  /* 1: lights itself; 2: charred, a third of the light */
} torch_part;

static float hash01(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return (float)(v >> 8) / 16777216.0f;
}

static entity_instance place(const torch_part *p, const float at[3], quat rot, float size, float light)
{
    entity_instance e = {0};
    const float local[3] = {p->pos[0] * size, p->pos[1] * size, p->pos[2] * size};
    float r[3] = {0.0f, 0.0f, 0.0f};
    quat_apply(rot, local, r);
    for (int i = 0; i < 3; i++) {
        e.pos[i] = at[i] + r[i];
        e.scale[i] = p->size[i] * size;
    }
    entity_set_rot(&e, quat_norm(quat_mul(rot, quat_axis(0.0f, 1.0f, 0.0f, p->turn))));
    e.tex = p->tex;
    e.light = p->glows == 1 ? 1.0f : (p->glows == 2 ? light * 0.35f : light);
    if (p->glows == 1) e.tint |= ENTITY_GLOWS;
    return e;
}

int torch_model(const float at[3], quat rot, float size, double time, uint32_t seed, float light,
                entity_instance out[TORCH_PARTS])
{
    float ph = 6.2831853f * hash01(seed);
    float t = (float)fmod(time, 3600.0);
    /* The flame leans with a slow draught and its height breathes faster;
     * each layer lags the one below, so the tip moves most. */
    float lean = 0.012f * sinf(t * 2.3f + ph) + 0.006f * sinf(t * 5.7f + 2.0f * ph);
    float breathe = 1.0f + 0.08f * sinf(t * 9.1f + ph) + 0.05f * sinf(t * 14.3f + 3.0f * ph);
    uint32_t bark = (uint32_t)tree_tex_bark(TREE_OAK, 0) * 0x010101u | ENTITY_TEX_METRIC;
    uint32_t chark = (uint32_t)T_GRAPHITE_ORE * 0x010101u | ENTITY_TEX_METRIC;
    uint32_t ember = (uint32_t)T_LAVA * 0x010101u;
    torch_part parts[TORCH_PARTS] = {
        /* Handle: a 9 cm stave, 55 cm long. */
        {{0.0f, 0.275f, 0.0f}, {0.09f, 0.55f, 0.09f}, 0.0f, bark, 0},
        /* Head: pitch-soaked wrapping, charred black, a little wider. */
        {{0.0f, 0.60f, 0.0f}, {0.13f, 0.11f, 0.13f}, 0.4f, chark, 2},
        /* Ember: the glowing top of the head. */
        {{0.0f, 0.655f, 0.0f}, {0.10f, 0.03f, 0.10f}, 0.4f, ember, 1},
        /* Flame: a solid hot core, then two layers of tongues around and
         * above it, narrowing upward and turned 30 degrees apart. */
        {{lean * 0.5f, 0.71f, 0.0f}, {0.07f, 0.09f * breathe, 0.07f}, 0.8f, ember, 1},
        {{lean * 0.3f, 0.73f, 0.0f}, {0.15f, 0.15f * breathe, 0.15f}, 0.0f, (uint32_t)T_FIRE0 * 0x010101u, 1},
        {{lean * 1.5f, 0.80f + 0.03f * (breathe - 1.0f), lean * 0.5f},
         {0.10f, 0.14f * breathe, 0.10f},
         0.52f,
         (uint32_t)(T_FIRE0 + 2) * 0x010101u,
         1},
    };
    /* Sparks: each rises 35 cm over 1.4 s, drifting and shrinking, then
     * starts again from the flame; the two are half a cycle apart. */
    for (int k = 0; k < 2; k++) {
        float c = fmodf(t / 1.4f + 0.5f * (float)k + hash01(seed + 1u + (uint32_t)k), 1.0f);
        float s = 0.025f * (1.0f - c);
        float dx = 0.04f * sinf(c * 5.0f + ph + (float)k * 2.1f), dz = 0.04f * cosf(c * 4.0f + ph * 1.3f);
        parts[6 + k] = (torch_part){{dx + lean * 2.0f, 0.80f + 0.35f * c, dz}, {s, s, s}, c * 3.0f, ember, 1};
    }
    for (int i = 0; i < TORCH_PARTS; i++) out[i] = place(&parts[i], at, rot, size, light);
    return TORCH_PARTS;
}
