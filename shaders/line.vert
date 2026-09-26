#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_vert.glsl"

layout(location = 0) in vec3 v_pos;

void main()
{
    gl_Position = pc.view_proj * vec4(v_pos + pc.origin.xyz, 1.0);
}
