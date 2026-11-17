#version 450

precision highp float;
precision highp int;

// A round point, added to the sky: the quad's corners fade out (no
// discard), and a large one (the moon) darkens a little toward its limb.
layout(location = 0) in mediump vec3 f_col;
layout(location = 1) in mediump vec2 f_uv;

layout(location = 0) out vec4 o_color;

void main()
{
    float r = dot(f_uv, f_uv);
    o_color = vec4(f_col * (clamp((1.0 - r) * 10.0, 0.0, 1.0) * (1.0 - 0.25 * r)), 0.0);
}
