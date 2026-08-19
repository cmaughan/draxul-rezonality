#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(set = 1, binding = 0) uniform sampler2D A;
void main() { fragColor = texture(A, uv); }
