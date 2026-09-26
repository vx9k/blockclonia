// Vertex-stage push constants, bytes [0, 96). Split from the fragment range
// so a draw that only changes the origin never touches fragment state.
layout(push_constant) uniform PushVert {
    mat4 view_proj;  // rotation + projection only (camera-relative world)
    vec4 origin;     // xyz: draw origin relative to the camera (lines)
    vec4 fog;        // x: fog start, y: 1 / (fog end - fog start), z: time in s, w: daylight 0..1
} pc;

// Texture layers the shaders animate (block.h tex_id; checked by a static
// assert in renderer.c).
const uint FIRE_FIRST = 32u;
const uint WATER_LAYER = 14u;
const uint LEAVES_LAYER = 8u;
