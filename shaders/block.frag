#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_frag.glsl"

precision highp float;
precision highp int;

layout(set = 0, binding = 0) uniform mediump sampler2DArray u_tex;
// Light (light.h): rgb block light, a sky light, 0..1 for levels 0..15.
layout(set = 0, binding = 1) uniform mediump sampler3D u_light;

layout(location = 0) in vec2 f_uv;  // highp: values reach 16 and pick exact texels
layout(location = 1) flat in uint f_layer;
layout(location = 2) in mediump vec2 f_shade;  // x: shade, y: fog
layout(location = 3) in vec3 f_luv;            // highp: hundreds of texels across

// Each light level is 80% as bright as the one above, so level L of 15 is
// 0.8^(15 - 15 l) for l = L / 15: exp2(K l - K) with K = -15 log2(0.8).
// One fused multiply-add and an exp2 per channel, at half precision on
// GPUs that have it.
const mediump float LIGHT_K = 4.8289214;

layout(location = 0) out vec4 o_color;

void main()
{
    // No discard anywhere: keeps early-Z intact on tile-based mobile GPUs.
    vec2 uv = f_uv;
    if (pc.color.a > 0.5) {
        // Filtered sampler. Anisotropic samplers magnify linearly on most
        // GPUs whatever magFilter says, which blurs pixel art up close, so
        // the coordinate is held at the texel centre except within one
        // screen pixel of a texel edge: crisp texels with smooth edges.
        // Where texels are smaller than a pixel (box = 1) uv is unchanged.
        const float TEXELS = 16.0;  // TEX_SIZE: texels per block
        vec2 t = f_uv * TEXELS;
        vec2 box = clamp(fwidth(t), 1e-4, 1.0);
        vec2 tx = t - 0.5 * box;
        uv = (floor(tx) + 0.5 + clamp((fract(tx) - (1.0 - box)) / box, 0.0, 1.0)) / TEXELS;
    }
    mediump vec4 c = texture(u_tex, vec3(uv, float(f_layer)));
    // Coloured block light and white sky light, the brighter per channel.
    // Daylight scales the sky's brightness, which on this curve is a shift
    // of its level (pc.light.x = log2(daylight) / K), so the comparison is
    // made on levels and only three channels pay for an exp2.
    mediump vec4 l = texture(u_light, f_luv);
    mediump vec3 light = exp2(max(l.rgb, vec3(l.a + pc.light.x)) * LIGHT_K - LIGHT_K) * f_shade.x;
    o_color = vec4(mix(c.rgb * light, pc.color.rgb, f_shade.y), c.a);
}
