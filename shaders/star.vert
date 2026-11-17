#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_vert.glsl"

// Stars and the moon (sky.h): one instance each, a quad of six vertices
// at the far plane. Push constants for this draw: origin is the star
// field's rotation (a quaternion; identity for the moon), fog.x how much
// of the night sky shows, fog.y 1 / aspect, fog.z the time in s.
layout(location = 0) in vec4 i_dir;  // xyz: direction, w: half size, pixels of a 1080-line screen
layout(location = 1) in vec4 i_col;  // rgb: colour times brightness, a: twinkle phase (< 0: steady)

layout(location = 0) out mediump vec3 f_col;
layout(location = 1) out mediump vec2 f_uv;

const vec2 CORNER[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0),
                              vec2(-1.0, -1.0));

vec3 qrot(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main()
{
    vec2 corner = CORNER[gl_VertexIndex];
    vec3 d = qrot(pc.origin, i_dir.xyz);
    // A direction (w = 0) projects to the far plane: depth 0 with reversed
    // Z, so the depth test passes only where the sky was left clear.
    vec4 c = pc.view_proj * vec4(d, 0.0);
    c.xy += corner * vec2(pc.fog.y, 1.0) * (i_dir.w * (2.0 / 1080.0)) * c.w;
    c.z = 0.0;
    gl_Position = c;
    // Haze swallows stars near the horizon; the air's turbulence makes
    // them twinkle, each at its own rate.
    float haze = smoothstep(-0.02, 0.25, d.y);
    float tw = i_col.a < 0.0 ? 1.0 : 0.7 + 0.3 * sin(pc.fog.z * (2.0 + 5.0 * i_col.a) + 40.0 * i_col.a);
    f_col = i_col.rgb * (pc.fog.x * haze * tw);
    f_uv = corner;
}
