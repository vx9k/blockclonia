// Fragment-stage push constants, bytes [96, 128).
layout(push_constant) uniform PushFrag {
    layout(offset = 96) vec4 color;  // rgb: fog colour (blocks) or line colour (lines); a: 1 to filter (block.frag)
    vec3 sky;   // block.frag: the sky's level shift per channel, log2(brightness) / LIGHT_K
    // packUnorm4x8 of the held light's level at the eye (rgb) and the darkest
    // level (a); a uint, as bits in a float could be a denormal a driver flushes
    uint held;
} pc;
