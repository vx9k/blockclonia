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

void main()
{
    vec3 p = vec3(float(v_data & 31u), float((v_data >> 5) & 31u), float((v_data >> 10) & 31u));
    uint face = (v_data >> 15) & 7u;
    uint ao = (v_data >> 18) & 3u;
    p.y -= float((v_data >> 28) & 15u) * 0.125;

    vec3 rel = p + i_origin;
    gl_Position = pc.view_proj * vec4(rel, 1.0);

    // Texture coordinates come from the position, so greedy-merged quads
    // tile the texture with a REPEAT sampler at no extra vertex cost.
    f_uv = face < 2u ? vec2(p.z, -p.y) : (face < 4u ? p.xz : vec2(p.x, -p.y));
    f_layer = (v_data >> 20) & 255u;
    f_light_fog = vec2(FACE_SHADE[face] * AO_CURVE[ao], clamp((length(rel) - pc.fog.x) * pc.fog.y, 0.0, 1.0));
}
