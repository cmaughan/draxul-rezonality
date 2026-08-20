#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "neon_environment.glsl"

layout (location = 0) in vec3 outRay;
layout (location = 0) out vec4 outFragColor;

void main()
{
    vec3 color = neonEnvironment(outRay, ubo.iTime * 2.0);
    color = vec3(1.0) - exp(-color * 1.28);
    outFragColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
