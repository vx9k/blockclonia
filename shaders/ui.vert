#version 450

// 2D overlay: positions in framebuffer pixels, colours sRGB-encoded.
layout(push_constant) uniform UiPush {
    vec2 scale;   // 2 / framebuffer size
    vec2 offset;  // -1, -1
    float linear_out;  // 1 when the swapchain encodes sRGB itself
} pc;

layout(location = 0) in vec2 v_pos;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;

layout(location = 0) out vec2 f_uv;
layout(location = 1) out mediump vec4 f_color;

void main()
{
    gl_Position = vec4(v_pos * pc.scale + pc.offset, 0.0, 1.0);
    f_uv = v_uv;
    vec3 c = v_color.rgb;
    float a = v_color.a;
    // Cheap sRGB to linear (max error under 1%): blending happens in
    // linear space on sRGB swapchains. Alpha is reshaped to match: a 94%
    // black overlay should leave 6% of the brightness you see, not 6% of
    // the light, which reads as barely dimmed.
    if (pc.linear_out > 0.5) {
        c = c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878);
        a = 1.0 - pow(1.0 - a, 2.2);
    }
    f_color = vec4(c, a);
}
