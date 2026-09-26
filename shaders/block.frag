#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(set = 0, binding = 0) uniform sampler2DArray u_tex;

layout(location = 0) in vec3 f_uvl;
layout(location = 1) in float f_light;
layout(location = 2) in float f_fog;

layout(location = 0) out vec4 o_color;

void main()
{
    // No discard anywhere: keeps early-Z intact on tile-based mobile GPUs.
    vec4 c = texture(u_tex, f_uvl);
    o_color = vec4(mix(c.rgb * f_light, pc.color.rgb, f_fog), c.a);
}
