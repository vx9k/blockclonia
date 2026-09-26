#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_vert.glsl"

// Falling blocks: a static unit cube (block vertex packing) instanced once
// per body.
layout(location = 0) in uint v_data;
layout(location = 1) in vec3 i_pos;  // body min corner relative to the camera
layout(location = 2) in uint i_tex;  // texture layers: side | top << 8 | bottom << 16

layout(location = 0) out vec2 f_uv;
layout(location = 1) flat out uint f_layer;
layout(location = 2) out mediump vec2 f_light_fog;

const float FACE_SHADE[6] = float[](0.80, 0.80, 1.00, 0.50, 0.65, 0.65);

void main()
{
    vec3 p = vec3(float(v_data & 31u), float((v_data >> 5) & 31u), float((v_data >> 10) & 31u));
    uint face = (v_data >> 15) & 7u;
    vec3 rel = i_pos + p;
    gl_Position = pc.view_proj * vec4(rel, 1.0);
    f_uv = face < 2u ? vec2(p.z, -p.y) : (face < 4u ? p.xz : vec2(p.x, -p.y));
    f_layer = face == 2u ? (i_tex >> 8) & 255u : (face == 3u ? (i_tex >> 16) & 255u : i_tex & 255u);
    f_light_fog = vec2(FACE_SHADE[face], clamp((length(rel) - pc.fog.x) * pc.fog.y, 0.0, 1.0));
}
