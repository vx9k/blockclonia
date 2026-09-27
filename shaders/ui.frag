#version 450

precision highp float;
precision highp int;

layout(set = 0, binding = 0) uniform mediump sampler2D u_font;

layout(location = 0) in vec2 f_uv;
layout(location = 1) in mediump vec4 f_color;

layout(location = 0) out vec4 o_color;

void main()
{
    o_color = vec4(f_color.rgb, f_color.a * texture(u_font, f_uv).r);
}
