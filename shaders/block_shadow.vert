#version 450
#extension GL_GOOGLE_include_directive : require
// The terrain in the sun: block.vert plus the shadow map's coordinate.
#define SHADOWS
#include "block_vert.glsl"
