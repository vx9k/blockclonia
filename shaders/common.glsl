// Shared push constant block. 112 bytes, inside the 128-byte minimum every
// Vulkan implementation guarantees, so no uniform buffers are needed.
layout(push_constant) uniform PC {
    mat4 view_proj;  // rotation + projection only (camera-relative world)
    vec4 origin;     // xyz: draw origin relative to the camera
    vec4 fog;        // x: fog start, y: 1 / (fog end - fog start)
    vec4 color;      // rgb: fog colour (blocks) or line colour (lines)
} pc;
