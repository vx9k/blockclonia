// Fragment-stage push constants, bytes [96, 112).
layout(push_constant) uniform PushFrag {
    layout(offset = 96) vec4 color;  // rgb: fog colour (blocks) or line colour (lines)
} pc;
