#version 450
#extension GL_GOOGLE_include_directive : require
// The terrain in the sun: block.frag plus the shadow map's compare.
#define SHADOWS
#include "block_frag.glsl"
