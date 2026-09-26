#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_frag.glsl"

precision highp float;
precision highp int;

layout(set = 0, binding = 0) uniform mediump sampler2DArray u_tex;

layout(location = 0) in vec2 f_uv;  // highp: values reach 16 and pick exact texels
layout(location = 1) flat in uint f_layer;
layout(location = 2) in mediump vec2 f_light_fog;  // x: light, y: fog

layout(location = 0) out vec4 o_color;

void main()
{
    // No discard anywhere: keeps early-Z intact on tile-based mobile GPUs.
    mediump vec4 c = texture(u_tex, vec3(f_uv, float(f_layer)));
    o_color = vec4(mix(c.rgb * f_light_fog.x, pc.color.rgb, f_light_fog.y), c.a);
}
