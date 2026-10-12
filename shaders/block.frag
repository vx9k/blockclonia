#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_frag.glsl"

precision highp float;
precision highp int;

layout(set = 0, binding = 0) uniform mediump sampler2DArray u_tex;

layout(location = 0) in vec2 f_uv;  // highp: values reach 16 and pick exact texels
layout(location = 1) flat in uint f_layer;
layout(location = 2) in mediump vec2 f_light_fog;  // x: light, y: fog

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
    o_color = vec4(mix(c.rgb * f_light_fog.x, pc.color.rgb, f_light_fog.y), c.a);
}
