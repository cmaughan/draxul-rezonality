#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "kelp_common.glsl"

// The ocean grid reused as a camera-centred seabed heightfield.
layout(location = 0) in vec4 inPos;
layout(location = 0) out vec3 worldPos;

void main()
{
    vec3 cam = cameraOrigin();
    vec2 xz = inPos.xz + cam.xz;
    worldPos = vec3(xz.x, seabedHeight(xz), xz.y);
    gl_Position = ubo.projection * ubo.view * vec4(worldPos, 1.0);
}
