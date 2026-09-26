#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// Falling blocks: float position (camera-relative) plus the block vertex
// packing for face, texture layer and the unit-cube corner (for UVs).
layout(location = 0) in vec3 v_pos;
layout(location = 1) in uint v_data;

layout(location = 0) out vec3 f_uvl;
layout(location = 1) out float f_light;
layout(location = 2) out float f_fog;

const float FACE_SHADE[6] = float[](0.80, 0.80, 1.00, 0.50, 0.65, 0.65);

void main()
{
    vec3 p = vec3(float(v_data & 31u), float((v_data >> 5) & 31u), float((v_data >> 10) & 31u));
    uint face = (v_data >> 15) & 7u;
    uint layer = (v_data >> 20) & 255u;
    gl_Position = pc.view_proj * vec4(v_pos, 1.0);
    vec2 uv = face < 2u ? vec2(p.z, -p.y) : (face < 4u ? p.xz : vec2(p.x, -p.y));
    f_uvl = vec3(uv, float(layer));
    f_light = FACE_SHADE[face];
    f_fog = clamp((length(v_pos) - pc.fog.x) * pc.fog.y, 0.0, 1.0);
}
