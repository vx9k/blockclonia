#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_vert.glsl"

// One packed uint per vertex; layout documented in src/mesher.h.
layout(location = 0) in uint v_data;
// Section min corner relative to the camera, one per draw (firstInstance).
layout(location = 1) in vec3 i_origin;

layout(location = 0) out vec2 f_uv;
layout(location = 1) flat out uint f_layer;
layout(location = 2) out mediump vec2 f_light_fog;

const float FACE_SHADE[6] = float[](0.80, 0.80, 1.00, 0.50, 0.65, 0.65);
const float AO_CURVE[4] = float[](0.45, 0.65, 0.82, 1.00);
const float TAU = 6.2831853;

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

void main()
{
    vec3 p = vec3(float(v_data & 31u), float((v_data >> 5) & 31u), float((v_data >> 10) & 31u));
    uint face = (v_data >> 15) & 7u;
    uint ao = (v_data >> 18) & 3u;
    uint drop = v_data >> 28;
    uint layer = (v_data >> 20) & 255u;
    p.y -= float(drop) * 0.125;

    vec3 rel = p + i_origin;
    vec3 world = rel + pc.origin.xyz;
    float t = pc.fog.z;
    // Night: daylight scales everything lit by the sky; face shade and AO stay.
    float light = FACE_SHADE[face] * AO_CURVE[ao] * pc.fog.w;
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
    } else if (layer == LEAVES_LAYER) {
        // Leaves sway 3 cm on an ellipse, 0.42 Hz (2.4 s), a wind gust
        // travelling across the canopy: phase from the position, 8 cycles
        // per 256 m in x, 5 in z, and one per 32 m up.
        float ph = TAU * (fract(t / 2.4) + dot(world.xz, vec2(8.0, 5.0) / 256.0) + world.y / 32.0);
        rel.xz += vec2(0.03 * sin(ph), 0.02 * cos(ph));
    } else if (layer - FIRE_FIRST < 4u) {
        // Flames: a flip-book at 8 frames/s, full brightness day and night.
        layer = FIRE_FIRST + (uint(t * 8.0) + layer) % 4u;
        light = 1.0;
    }
    gl_Position = pc.view_proj * vec4(rel, 1.0);

    // Texture coordinates come from the position, so greedy-merged quads
    // tile the texture with a REPEAT sampler at no extra vertex cost.
    f_uv = (face < 2u ? vec2(p.z, -p.y) : (face < 4u ? p.xz : vec2(p.x, -p.y))) + scroll;
    f_layer = layer;
    f_light_fog = vec2(light, clamp((length(rel) - pc.fog.x) * pc.fog.y, 0.0, 1.0));
}
