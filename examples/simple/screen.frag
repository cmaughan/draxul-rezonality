#version 450
#extension GL_GOOGLE_include_directive : enable

layout(location = 0) out vec4 fragColor;
void main()
{
    fragColor = vec4(0.85, 0.15, 0.1, 1.0);
}
