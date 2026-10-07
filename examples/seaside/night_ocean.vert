#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "night_common.glsl"
#include "waves.glsl"

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
    // Swell shoals and steepens toward the rocks.
    float shore = shoreDistance(xz);
    float shoal = 1.0 + 0.5 * smoothstep(14.0, 2.0, shore);
    vec3 d = gerstnerDisplace(xz, t, lod) * (1.0 - far) * shoal;
    worldPos = vec3(xz.x + d.x, d.y, xz.y + d.z);
    gridXZ = xz;
    gl_Position = ubo.projection * ubo.view * vec4(worldPos, 1.0);
}
