// Vertex-stage push constants, bytes [0, 96). Split from the fragment range
// so a draw that only changes the origin never touches fragment state.
layout(push_constant) uniform PC {
    mat4 view_proj;  // rotation + projection only (camera-relative world)
    vec4 origin;     // xyz: draw origin relative to the camera (lines)
    vec4 fog;        // x: fog start, y: 1 / (fog end - fog start)
} pc;
