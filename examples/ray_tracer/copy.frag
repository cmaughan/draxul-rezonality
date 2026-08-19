#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;
layout(set = 1, binding = 0) uniform sampler2D RayTraceTarget;

void main()
{
    outFragColor = texture(RayTraceTarget, inUV);
    outFragColor.a = 1.0;
}
