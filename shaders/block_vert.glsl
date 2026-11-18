// The terrain's vertex shader; block.vert compiles it, block_shadow.vert
// with SHADOWS defined for the sun's shadow map (shadow.h).
#include "push_vert.glsl"

// One packed uint per vertex; layout documented in src/mesher.h.
layout(location = 0) in uint v_data;
// Section min corner relative to the camera, one per draw (firstInstance).
layout(location = 1) in vec3 i_origin;
// The section's light slot: its texel offset in the atlas, x | y << 11 | z << 22.
layout(location = 2) in uint i_light;

layout(location = 0) out vec2 f_uv;
// Texture layer | sun shading << 16 (9 bits) | a tree's tint << 25 (4 bits).
layout(location = 1) flat out uint f_layer;
layout(location = 2) out mediump vec2 f_shade;  // x: shade, y: fog
layout(location = 3) out vec3 f_luv;            // light atlas coordinate
#ifdef SHADOWS
// The shadow map's matrix (shadow_matrix): camera-relative world to its
// clip space. A uniform, as the push constants are full; per frame, at
// the frame's dynamic offset.
layout(set = 1, binding = 0) uniform ShadowMap { mat4 clip; } u_sh;
// The map's coordinate and the depth to compare; w: the light shafts'
// strength (block_frag.glsl), in the third varying slot's spare component.
layout(location = 4) out vec4 f_sh;
#endif

// Faces 6 and 7 are a plant's crossed quads, lit from all round.
const float FACE_SHADE[8] = float[](0.80, 0.80, 1.00, 0.50, 0.65, 0.65, 0.85, 0.85);
const float AO_CURVE[4] = float[](0.45, 0.65, 0.82, 1.00);
const float TAU = 6.2831853;
// The sun shades the sky's light on faces turned from it (lightfx.h): a
// face keeps 1 - SUN_SHADE times the contrast of its light. The sun runs
// in a plane tilted by the latitude, so its z is its y times tan(40 deg)
// and the push constants carry only x and y (times the contrast).
const float SUN_SHADE = 0.4;  // LIGHTFX_SUN_SHADE
#ifdef SHADOWS
// With shadows, out of the sun (shadowed or turned from it) a face keeps
// 30% of the sky's light at full contrast: on a clear day direct sun is
// most of the light, and skylight alone leaves shade about a third as
// bright. Without them, a gentler 60% stands in, since nothing is shadowed.
const float FACE_SUN = 0.7;
#else
const float FACE_SUN = SUN_SHADE;
#endif
const float SUN_Z = 0.8391;
const float LIGHT_K = 4.8289214;  // LIGHT_CURVE_K
const float SUN_STEPS = 1024.0;  // 1 / LIGHTFX_SUN_STEP
#ifdef SHADOWS
// Light shafts: the haze's extinction per metre (half the way at
// 64 m, the map's edge) and its brightest in-scatter, at full contrast.
const float HAZE = 0.01;
const float SHAFT = 0.6;
#endif

// Animation is a function of the world position, which is rel + pc.origin
// with x and z wrapped to a 256 m tile, and of pc.fog.z, time wrapped to
// 3600 s. So every spatial period divides 256 m (whole wave numbers per
// tile) and every time period divides 3600 s, and neither wrap shows.
//
// Water swell: three deep-water waves, wave numbers n (cycles per 256 m)
// with frequencies from the dispersion relation w^2 = g k (g = 9.81 m/s^2,
// k = 2 pi |n| / 256 m), rounded to whole cycles per hour: n = (3, 1)
// 81 m, 7.2 s; (-2, 3) 71 m, 6.7 s; (1, -4) 62 m, 6.3 s. Amplitudes sum to
// 3 cm, well inside the 12.5 cm a surface already sits below the block top.
const vec2 WAVE_N1 = vec2(3.0, 1.0) / 256.0;
const vec2 WAVE_N2 = vec2(-2.0, 3.0) / 256.0;
const vec2 WAVE_N3 = vec2(1.0, -4.0) / 256.0;
const vec3 WAVE_F = vec3(500.0, 534.0, 571.0) / 3600.0;  // Hz
const vec3 WAVE_A = vec3(0.015, 0.010, 0.005);             // m

// Trees of a kind are not all one colour: soil, light and age make some
// paler, some darker and some yellower. A smooth field over the ground,
// 8 m cells (32 to the 256 m tile), gives each tree its own, and blocks
// of one tree nearly the same: 1..15, 0 being no tint.
uint tree_tint(vec2 xz)
{
    vec2 g = xz / 8.0, f = fract(g);
    ivec2 i = ivec2(floor(g));
    f = f * f * (3.0 - 2.0 * f);
    float v[4];
    for (int k = 0; k < 4; k++) {
        uvec2 c = uvec2((i + ivec2(k & 1, k >> 1)) & 31);
        uint h = c.x * 0x9E3779B1u ^ c.y * 0x85EBCA77u;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        v[k] = float(h & 0xFFFFu) / 65535.0;
    }
    float t = mix(mix(v[0], v[1], f.x), mix(v[2], v[3], f.x), f.y);
    return 1u + uint(t * 14.99);
}

void main()
{
    vec3 p = vec3(float(v_data & 31u), float((v_data >> 5) & 31u), float((v_data >> 10) & 31u));
    uint face = (v_data >> 15) & 7u;
    uint ao = (v_data >> 18) & 3u;
    uint drop = v_data >> 28;
    uint layer = (v_data >> 20) & 255u;

    // Light comes from the cell in front of the face: half a cell out along
    // the normal is that cell's centre, and along the face the corner lies
    // between texel centres, so the linear filter blends the four cells
    // around each corner (smooth lighting) and never the solid cell behind.
    // The slot's border (+1) holds the neighbours. Texels are stored x, z, y.
    // The coordinate is affine in the position, so it is computed here and
    // interpolated, and the fragment shader does one fetch and no maths.
    // Intel interpolates in the fragment shader, so the three components
    // cost about 0.2 ms a frame on its iGPU at 32 columns, but sending the
    // coordinate flat and rebuilding it there from f_uv measured slower.
    vec3 n = vec3(equal(uvec3(face >> 1), uvec3(0u, 1u, 2u))) * (0.5 - float(face & 1u));
    vec3 q = p + 1.0 + n;
    // A plant (n is zero) takes its light half a cell up, from its own
    // cell and the one above, not from the soil it stands in.
    bool plant = face >= 6u;
    q.y += plant ? 0.5 : 0.0;
    vec3 base = vec3(float(i_light & 2047u), float((i_light >> 11) & 2047u), float(i_light >> 22));
    f_luv = (base + q.xzy) * vec3(LIGHT_INV_X, LIGHT_INV_Y, LIGHT_INV_Z);
    p.y -= float(drop) * 0.125;

    vec3 rel = p + i_origin;
    vec3 world = rel + pc.origin.xyz;
    // Time past 4096 s means a still world (the sway setting is off):
    // leaves and plants stand, water and flames run as ever.
    float t = pc.fog.z, sway = 1.0;
    if (t >= 4096.0) {
        t -= 4096.0;
        sway = 0.0;
    }
    // Face shade and AO darken both kinds of light; daylight scales only the sky's.
    float shade = FACE_SHADE[face] * AO_CURVE[ao];
    vec2 scroll = vec2(0.0);
    if (layer == WATER_LAYER) {
        // Only vertices on a surface's top edge carry a drop; the side
        // faces of a surface block share them, so tops and sides move
        // together and never open a gap.
        if (drop != 0u) {
            vec3 ph = vec3(dot(world.xz, WAVE_N1), dot(world.xz, WAVE_N2), dot(world.xz, WAVE_N3)) + fract(t * WAVE_F);
            rel.y += dot(WAVE_A, sin(TAU * ph));
        }
        // The texture drifts one tile per 40 s along x and per 60 s along
        // the other axis (2.5 and 1.7 cm/s).
        scroll = fract(t * vec2(1.0 / 40.0, 1.0 / 60.0));
    } else if (layer - TREE_LEAVES_FIRST < TREE_KINDS) {
        // Leaves sway 3 cm on an ellipse, 0.42 Hz (2.4 s), a wind gust
        // travelling across the canopy: phase from the position, 8 cycles
        // per 256 m in x, 5 in z, and one per 32 m up.
        float ph = TAU * (fract(t / 2.4) + dot(world.xz, vec2(8.0, 5.0) / 256.0) + world.y / 32.0);
        rel.xz += sway * vec2(0.03 * sin(ph), 0.02 * cos(ph));
    } else if (layer - PLANT_FIRST < PLANT_COUNT) {
        // Plants bend from the foot: only the top edge (AO 3) moves, 8 cm
        // in the same gust as the leaves, a little faster for a light stem.
        // Vines hang, so theirs is the free bottom edge.
        float ph = TAU * (fract(t / 2.0) + dot(world.xz, vec2(8.0, 5.0) / 256.0));
        rel.xz += sway * float((ao == 3u) != (layer == VINES_LAYER)) * vec2(0.08 * sin(ph), 0.05 * cos(ph));
    } else if (layer - FIRE_FIRST < 4u) {
        // Flames: a flip-book at 8 frames/s, full brightness day and night.
        layer = FIRE_FIRST + (uint(t * 8.0) + layer) % 4u;
        shade = 1.0;
        f_luv = LIGHT_FULL;
    }
    gl_Position = pc.view_proj * vec4(rel, 1.0);

    // Texture coordinates come from the position, so greedy-merged quads
    // tile the texture with a REPEAT sampler at no extra vertex cost.
    f_uv = (face < 2u ? vec2(p.z, -p.y) : (face < 4u ? p.xz : vec2(p.x, -p.y))) + scroll;
    // As a shift of the sky's level, which block.frag adds before its one
    // exp2: flat, so it costs no interpolation, in bits f_layer has spare.
    vec3 sun = vec3(pc.origin.w, pc.fog.w, pc.fog.w * SUN_Z);
    // A plant takes the sun as a top face does: its blades face every way.
    vec3 ns = plant ? vec3(0.0, 0.5, 0.0) : n;
    float m = 1.0 - FACE_SUN * (length(sun) - max(dot(2.0 * ns, sun), 0.0));
    f_layer = layer | uint(-log2(m) * (SUN_STEPS / LIGHT_K) + 0.5) << 16;
    // A tree's tint, at the provoking vertex: the field hardly changes in
    // the metre between a block's corners.
    if (layer - TREE_FIRST < TREE_COUNT) f_layer |= tree_tint(world.xz) << 25;
#ifdef SHADOWS
    // Out of the sun a face keeps the sky light of one turned from it:
    // the extra shift, in pairs of steps in bits 8-15 (layers fit in 8; at
    // most 368 steps, 184 pairs).
    float turned = 1.0 - FACE_SUN * length(sun);
    f_layer |= uint(max(log2(m) - log2(turned), 0.0) * (SUN_STEPS / LIGHT_K) * 0.5 + 0.5) << 8;
    vec4 s = u_sh.clip * vec4(rel, 1.0);
    // Light shafts: sunlit haze between the eye and the face, brighter
    // looking toward the light (forward scattering) and thicker with
    // distance; the fragment shader finds how much of the way is lit.
    float dist = length(rel), c = length(sun);
    float toward = c > 0.0 && dist > 0.0 ? max(dot(rel, sun) / (dist * c), 0.0) : 0.0;
    float t2 = toward * toward;
    float phase = 0.15 + 0.85 * t2 * t2 * t2;
    f_sh = vec4(s.xy * 0.5 + 0.5, s.z, SHAFT * c * phase * (1.0 - exp(-HAZE * dist)));
#endif
    // Eight components to interpolate (uv 2, layer 1, shade and fog 2,
    // light 3) fill two vec4 slots; daylight is applied per pixel instead
    // of costing a ninth, which Intel pays for with a third slot.
    f_shade = vec2(shade, clamp((length(rel) - pc.fog.x) * pc.fog.y, 0.0, 1.0));
}
