#version 450

precision highp float;
precision highp int;

// Bloom (bloom.h), at a quarter of the resolution. Mode 0 takes the
// glowing part of the scene, its colour times the glow its alpha marks,
// averaged over the 4x4 pixels under each texel by four bilinear fetches.
// Mode 1 blurs along pc.step with the kernel bloom_kernel folds into
// bilinear fetches; run across and then down, it is a Gaussian.
layout(constant_id = 0) const uint MODE = 0u;

layout(set = 0, binding = 0) uniform mediump sampler2D u_src;

layout(push_constant) uniform BloomPush {
    vec2 step;        // mode 0: one source pixel; 1: one texel along the blur
    float weight[5];  // centre, then each pair of taps
    float offset[5];  // texels from the centre
} pc;

layout(location = 0) in vec2 f_uv;

layout(location = 0) out vec4 o_color;

mediump vec3 glowing(vec2 uv)
{
    mediump vec4 s = texture(u_src, uv);
    return s.rgb * s.a;
}

void main()
{
    mediump vec3 c = vec3(0.0);
    if (MODE == 0u) {
        c = 0.25 * (glowing(f_uv - pc.step) + glowing(f_uv + pc.step) + glowing(f_uv + vec2(pc.step.x, -pc.step.y)) +
                    glowing(f_uv + vec2(-pc.step.x, pc.step.y)));
    } else {
        c = texture(u_src, f_uv).rgb * pc.weight[0];
        for (int i = 1; i < 5; i++) {
            vec2 o = pc.step * pc.offset[i];
            c += (texture(u_src, f_uv + o).rgb + texture(u_src, f_uv - o).rgb) * pc.weight[i];
        }
    }
    o_color = vec4(c, 1.0);
}
