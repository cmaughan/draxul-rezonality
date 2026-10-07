#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "sunset_sky.glsl"
#include "waves.glsl"

// Camera-centred polar grid displaced by the Gerstner wave train.
layout(location = 0) in vec4 inPos;

layout(location = 0) out vec3 worldPos;
layout(location = 1) out vec2 gridXZ;

void main()
{
    float t = sceneTime();
    vec3 cam = cameraOrigin();
    vec2 xz = inPos.xz + cam.xz;
    float r = length(inPos.xz);
    float lod = smoothstep(50.0, 220.0, r);
    float far = smoothstep(260.0, 800.0, r);
    vec3 d = gerstnerDisplace(xz, t, lod) * (1.0 - far);
    worldPos = vec3(xz.x + d.x, d.y, xz.y + d.z);
    gridXZ = xz;
    gl_Position = ubo.projection * ubo.view * vec4(worldPos, 1.0);
}
