#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_vert.glsl"

// The sun's shadow map (shadow.h): depth only, no fragment shader. The
// section's vertices as block.vert reads them, through the map's matrix
// in view_proj. Waves and swaying leaves are left out: a few centimetres
// against 6 cm texels.
layout(location = 0) in uint v_data;
layout(location = 1) in vec3 i_origin;

void main()
{
    vec3 p = vec3(float(v_data & 31u), float((v_data >> 5) & 31u), float((v_data >> 10) & 31u));
    p.y -= float(v_data >> 28) * 0.125;
    gl_Position = pc.view_proj * vec4(p + i_origin, 1.0);
}
