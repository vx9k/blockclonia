#version 450
#extension GL_GOOGLE_include_directive : require
#include "push_frag.glsl"

precision highp float;
precision highp int;

// The sky's colour by direction. pc.color is the sky (and fog) colour the
// frame was cleared to, which the horizon keeps exactly so distant terrain
// fades into it without a seam. Overhead the air's path is short and the
// blue it scatters (Rayleigh, as 1 / wavelength^4) dominates, so the
// zenith is deeper and bluer; toward the sun haze scatters forward (Mie),
// a bright glow that turns warm while the sun is low and its light has
// crossed a long path of air.
layout(location = 0) in vec3 f_dir;
layout(location = 1) flat in vec4 f_sun;

layout(location = 0) out vec4 o_color;

// The zenith: a deep blue as bright as the sky (relative luminance), so a
// dusk sky that is orange at the horizon stays blue overhead.
const vec3 ZENITH = vec3(0.10, 0.28, 0.90);
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
const vec3 GLOW_HIGH = vec3(1.0, 0.95, 0.85);
const vec3 GLOW_LOW = vec3(1.0, 0.55, 0.25);

void main()
{
    vec3 d = normalize(f_dir);
    vec3 sky = pc.color.rgb;
    // Most of the change is near the horizon, where the path through the
    // air grows fastest with the angle.
    float up = sqrt(clamp(d.y, 0.0, 1.0));
    vec3 zenith = ZENITH * (0.7 * dot(sky, LUMA) / dot(ZENITH, LUMA));
    vec3 c = mix(sky, zenith, up);
    float mu = max(dot(d, f_sun.xyz), 0.0);
    float mu2 = mu * mu, mu4 = mu2 * mu2, mu8 = mu4 * mu4;
    float glow = 0.25 * mu8 + 0.6 * mu8 * mu8 * mu8 * mu8;
    float low = 1.0 - smoothstep(0.0, 0.35, f_sun.y);
    c += mix(GLOW_HIGH, GLOW_LOW, low) * (glow * f_sun.w);
    o_color = vec4(c, pc.color.a);
}
