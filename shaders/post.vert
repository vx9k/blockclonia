#version 450

// One triangle that covers the screen, from the vertex index alone (no
// vertex buffer): (0,0), (2,0), (0,2) in texture space.
layout(location = 0) out vec2 f_uv;

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    f_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
