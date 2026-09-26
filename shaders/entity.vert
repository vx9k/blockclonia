#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_vert.glsl"

// Entities: a static unit cube (block vertex packing) instanced once per
// falling block, dropped item, particle, crack overlay or view-model part.
// Each instance rotates (quaternion) and scales the cube about its centre.
layout(location = 0) in uint v_data;
layout(location = 1) in vec3 i_pos;    // centre relative to the camera
layout(location = 2) in uint i_tex;    // texture layers: side | top << 8 | bottom << 16
layout(location = 3) in vec4 i_rot;    // unit quaternion
layout(location = 4) in vec4 i_scale;  // xyz: size in metres, w: brightness

layout(location = 0) out vec2 f_uv;
layout(location = 1) flat out uint f_layer;
layout(location = 2) out mediump vec2 f_light_fog;

const float FACE_SHADE[6] = float[](0.80, 0.80, 1.00, 0.50, 0.65, 0.65);

vec3 qrot(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main()
{
    vec3 p = vec3(float(v_data & 31u), float((v_data >> 5) & 31u), float((v_data >> 10) & 31u));
    uint face = (v_data >> 15) & 7u;
    vec3 rel = i_pos + qrot(i_rot, (p - 0.5) * i_scale.xyz);
    gl_Position = pc.view_proj * vec4(rel, 1.0);
    f_uv = face < 2u ? vec2(p.z, -p.y) : (face < 4u ? p.xz : vec2(p.x, -p.y));
    f_layer = face == 2u ? (i_tex >> 8) & 255u : (face == 3u ? (i_tex >> 16) & 255u : i_tex & 255u);
    // Daylight scales world entities like the blocks around them. The view
    // model pass turns fog off (fog.y = 0) and brings its own brightness
    // in i_scale.w, so it is not darkened twice.
    float light = FACE_SHADE[face] * i_scale.w * (pc.fog.y > 0.0 ? pc.fog.w : 1.0);
    // Flames are a flip-book (fog.z carries the time), full brightness.
    if (f_layer - FIRE_FIRST < 4u) {
        f_layer = FIRE_FIRST + (uint(pc.fog.z * 8.0) + f_layer) % 4u;
        light = i_scale.w;
    }
    f_light_fog = vec2(light, clamp((length(rel) - pc.fog.x) * pc.fog.y, 0.0, 1.0));
}
