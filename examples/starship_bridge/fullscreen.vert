#version 450

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec2 uv;

void main()
{
    uv = inUV;
    gl_Position = inPos;
}
