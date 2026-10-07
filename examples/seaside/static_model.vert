#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"

// Pass-through for models that do not animate.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec3 worldPos;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec2 partUV;
layout(location = 3) out vec3 vertexColor;

void main()
{
    vec4 world = ubo.model * vec4(inPos.xyz, 1.0);
    worldPos = world.xyz;
    worldNormal = mat3(ubo.model) * inNormal;
    partUV = inUV;
    vertexColor = inColor;
    gl_Position = ubo.projection * ubo.view * world;
}
