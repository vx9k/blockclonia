#version 450
#extension GL_GOOGLE_include_directive : require
// Entities: block.frag with a colour per instance (entity.vert), so one
// greyscale material layer serves every metal, wood and dye of the item
// models (itemmodel.h). The terrain doesn't pay for the extra input.
#define TINTED
#include "block_frag.glsl"
