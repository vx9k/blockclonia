// Fragment-stage push constants, bytes [96, 128).
layout(push_constant) uniform PushFrag {
    layout(offset = 96) vec4 color;  // rgb: fog colour (blocks) or line colour (lines); a: 1 to filter (block.frag)
    vec4 light;                      // x: log2(daylight) / LIGHT_K, the sky's level shift (block.frag)
} pc;
