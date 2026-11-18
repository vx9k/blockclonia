// Vertex-stage push constants, bytes [0, 96). Split from the fragment range
// so a draw that only changes the origin never touches fragment state.
layout(push_constant) uniform PushVert {
    mat4 view_proj;  // rotation + projection only (camera-relative world)
    vec4 origin;     // xyz: draw origin relative to the camera (lines); w: the sun's x (block.vert)
    vec4 fog;        // x: fog start, y: 1 / (fog end - fog start), z: time in s, w: the sun's y
} pc;

// Texture layers the shaders animate (block.h tex_id; checked by a static
// assert in renderer.c).
const uint FIRE_FIRST = 22u;
const uint WATER_LAYER = 14u;
const uint LEAVES_LAYER = 8u;
const uint LEAVES_FIRST = 117u;  // the other trees' leaves, four of them
const uint PLANT_FIRST = 121u;   // the ground plants, eleven of them
const uint PLANT_COUNT = 11u;

// The light atlas's size in texels, inverted (renderer.c, light_plan): set
// when the pipeline is built, so turning a texel position into a texture
// coordinate costs one multiply.
layout(constant_id = 0) const float LIGHT_INV_X = 1.0;
layout(constant_id = 1) const float LIGHT_INV_Y = 1.0;
layout(constant_id = 2) const float LIGHT_INV_Z = 1.0;
// Slot 0 of the atlas is full light; this is a point inside it, for what
// brings its own brightness (flames, entities).
#define LIGHT_FULL (9.0 * vec3(LIGHT_INV_X, LIGHT_INV_Y, LIGHT_INV_Z))
