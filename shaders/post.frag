#version 450

precision highp float;
precision highp int;

// Blurred and double vision: the world frame, defocused, seen twice side by
// side when the eyes stop tracking together.
layout(set = 0, binding = 0) uniform mediump sampler2D u_scene;

layout(push_constant) uniform PostPush {
    vec2 texel;   // 1 / framebuffer size
    float radius; // defocus radius, pixels
    float ghost;  // distance between the two images, pixels
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
    vec2 g = vec2(0.5 * pc.ghost * pc.texel.x, 0.0);
    o_color = vec4(0.5 * (defocus(f_uv - g) + defocus(f_uv + g)), 1.0);
}
