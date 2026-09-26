#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) out vec4 o_color;

void main()
{
    o_color = vec4(pc.color.rgb, 1.0);
}
