#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_frag.glsl"

precision highp float;
precision highp int;

layout(set = 0, binding = 0) uniform mediump sampler2DArray u_tex;
// Light (light.h): rgb block light, a sky light, 0..1 for levels 0..15.
layout(set = 0, binding = 1) uniform mediump sampler3D u_light;

layout(location = 0) in vec2 f_uv;  // highp: values reach 16 and pick exact texels
layout(location = 1) flat in uint f_layer;  // layer | sun shading << 16 (block.vert)
layout(location = 2) in mediump vec2 f_shade;  // x: shade, y: fog
layout(location = 3) in vec3 f_luv;            // highp: hundreds of texels across

// Each light level is 80% as bright as the one above, so level L of 15 is
// 0.8^(15 - 15 l) for l = L / 15: exp2(K l - K) with K = -15 log2(0.8).
// One fused multiply-add and an exp2 per channel, at half precision on
// GPUs that have it.
const mediump float LIGHT_K = 4.8289214;
// lightfx.h: the sun's shading steps, block light's gain of 1.3 as a level
// shift (log2(1.3) / K), the tone curve's knee. The held light fades a
// level a metre from the eye, whose distance is ZNEAR / depth (reversed Z).
const mediump float SUN_STEP = 1.0 / 1024.0;
const mediump float BLOCK_GAIN = 0.07844;
const mediump float TONE_KNEE = 0.75;
const float HELD_FALL = 0.05 / 15.0;

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
    mediump vec4 c = texture(u_tex, vec3(uv, float(f_layer & 0xFFFFu)));
    // Coloured block light and sky light, the brighter per channel. The
    // sky's colour and the sun's shading scale its brightness, which on
    // this curve shifts its level, and the held light and the floor are
    // levels too, so everything is compared on levels and only three
    // channels pay for an exp2.
    mediump vec4 l = texture(u_light, f_luv);
    mediump vec4 held = unpackUnorm4x8(pc.held);
    mediump vec3 sky = pc.sky + (l.a - float(f_layer >> 16) * SUN_STEP);
    mediump vec3 blk = max(l.rgb, held.rgb - HELD_FALL / gl_FragCoord.z) + BLOCK_GAIN;
    mediump vec3 light = exp2(max(max(blk, sky), held.a) * LIGHT_K - LIGHT_K) * f_shade.x;
    // The tone curve: unchanged up to the knee, then rolling off toward
    // white, so a lamp-lit wall or lava saturates instead of clipping.
    mediump vec3 x = c.rgb * light;
    mediump vec3 u = max(x - TONE_KNEE, 0.0);
    x = min(x, TONE_KNEE) + u / (1.0 + u * (1.0 / (1.0 - TONE_KNEE)));
    o_color = vec4(mix(x, pc.color.rgb, f_shade.y), c.a);
}
