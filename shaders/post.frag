#version 450

precision highp float;
precision highp int;

// Blurred and double vision: the world frame, defocused, seen twice side by
// side when the eyes stop tracking together. Bloom (bloom.frag's blurred
// glow) is added on top.
layout(set = 0, binding = 0) uniform mediump sampler2D u_scene;
layout(set = 0, binding = 1) uniform mediump sampler2D u_bloom;

layout(push_constant) uniform PostPush {
    vec2 texel;   // 1 / framebuffer size
    float radius; // defocus radius, pixels
    float ghost;  // distance between the two images, pixels
    float bloom;  // how much of the glow is added (0: none)
} pc;

layout(location = 0) in vec2 f_uv;

layout(location = 0) out vec4 o_color;

// Nine bilinear taps: the centre, then a ring alternating between the full
// and half radius, so a wide radius reads as a soft disc rather than eight
// copies. Weak GPUs only pay for it while the player is concussed.
vec3 defocus(vec2 uv)
{
    vec2 r = pc.texel * pc.radius;
    vec3 c = texture(u_scene, uv).rgb * 0.2;
    c += texture(u_scene, uv + vec2(1.0, 0.0) * r).rgb * 0.1;
    c += texture(u_scene, uv + vec2(0.354, 0.354) * r).rgb * 0.1;
    c += texture(u_scene, uv + vec2(0.0, 1.0) * r).rgb * 0.1;
    c += texture(u_scene, uv + vec2(-0.354, 0.354) * r).rgb * 0.1;
    c += texture(u_scene, uv + vec2(-1.0, 0.0) * r).rgb * 0.1;
    c += texture(u_scene, uv + vec2(-0.354, -0.354) * r).rgb * 0.1;
    c += texture(u_scene, uv + vec2(0.0, -1.0) * r).rgb * 0.1;
    c += texture(u_scene, uv + vec2(0.354, -0.354) * r).rgb * 0.1;
    return c;
}

void main()
{
    vec3 c = vec3(0.0);
    if (pc.radius > 0.0 || pc.ghost > 0.0) {
        vec2 g = vec2(0.5 * pc.ghost * pc.texel.x, 0.0);
        c = 0.5 * (defocus(f_uv - g) + defocus(f_uv + g));
    } else {
        c = texture(u_scene, f_uv).rgb;  // bloom alone: one fetch
    }
    if (pc.bloom > 0.0) c += texture(u_bloom, f_uv).rgb * pc.bloom;
    o_color = vec4(c, 1.0);
}
