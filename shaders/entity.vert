#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_vert.glsl"

// Entities: a static unit cube (block vertex packing) instanced once per
// falling block, dropped item, particle, crack overlay or view-model part.
// Each instance rotates (quaternion) and scales the cube about its centre.
layout(location = 0) in uint v_data;
layout(location = 1) in vec3 i_pos;    // centre relative to the camera
layout(location = 2) in uint i_tex;    // layers: side | top << 8 | bottom << 16 | front << 24 | metric << 31
layout(location = 3) in vec4 i_rot;    // unit quaternion
layout(location = 4) in vec4 i_scale;  // xyz: size in metres, w: brightness
layout(location = 5) in uint i_tint;   // rgb subtracted from white (entity.h), so 0 is none

layout(location = 0) out vec2 f_uv;
layout(location = 1) flat out uint f_layer;
layout(location = 2) out mediump vec2 f_shade;  // as block.vert: shade, fog
layout(location = 3) out vec3 f_luv;
layout(location = 4) out mediump vec3 f_tint;

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
    // The front (-z) face can have its own layer: a creature's face.
    uint front = (i_tex >> 24) & 127u;
    if (face == 5u && front != 0u) f_layer = front;
    // Metric art (ENTITY_TEX_METRIC): 1 m of face is one 16-texel layer,
    // read from the layer's top-left, front and top first.
    if ((i_tex & 0x80000000u) != 0u) {
        vec2 fp = face < 2u ? vec2(p.z, 1.0 - p.y) : (face < 4u ? p.xz : vec2(p.x, 1.0 - p.y));
        vec2 size = face < 2u ? i_scale.zy : (face < 4u ? i_scale.xz : i_scale.xy);
        f_uv = fp * size;
    }
    // i_scale.w is the light where the entity is (light_at: sky light by
    // daylight, block light), so the atlas is read at its full slot.
    // block.frag gains block light by 1.3 (lightfx.h) and the full slot
    // reads as block light, so the entity's own light is divided by it.
    float light = FACE_SHADE[face] * i_scale.w * (1.0 / 1.3);
    // Flames are a flip-book (fog.z carries the time), full brightness.
    if (f_layer - FIRE_FIRST < 4u) {
        f_layer = FIRE_FIRST + (uint(pc.fog.z * 8.0) + f_layer) % 4u;
        light = i_scale.w;
    }
    f_shade = vec2(light, clamp((length(rel) - pc.fog.x) * pc.fog.y, 0.0, 1.0));
    f_luv = LIGHT_FULL;
    f_tint = 1.0 - unpackUnorm4x8(i_tint).rgb;
}
