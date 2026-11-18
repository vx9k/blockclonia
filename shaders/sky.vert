#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_vert.glsl"

// The sky's gradient (sky.frag): one triangle over the whole screen at the
// far plane, tested against the depth buffer so it fills only what the
// world left clear. Push constants for this draw: origin.xyz toward the
// sun (a unit vector), origin.w how much its glow shows.
layout(location = 0) out vec3 f_dir;
layout(location = 1) flat out vec4 f_sun;

void main()
{
    vec2 c = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    // The point of the near plane (depth 1, reversed Z) under this corner,
    // relative to the eye: the view direction. Points of a plane are affine
    // in screen space, so the interpolated direction is exact.
    vec4 p = inverse(pc.view_proj) * vec4(c, 1.0, 1.0);
    f_dir = p.xyz / p.w;
    f_sun = pc.origin;
    gl_Position = vec4(c, 0.0, 1.0);
}
