#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec3 inTangent;
layout(location = 5) in vec3 inBitangent;

layout(location = 0) out vec3 worldPos;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec2 partUV;
layout(location = 3) out vec3 objectPos;

// Slow evasive drift: yaw sweep, banking roll and a gentle bob.
mat3 shipAttitude(float t)
{
    float yaw = 2.15 + 0.5 * sin(t * 0.21);
    float roll = -0.16 * cos(t * 0.21) + 0.05 * sin(t * 1.3);
    float pitch = 0.08 * sin(t * 0.37);
    float cy = cos(yaw), sy = sin(yaw);
    float cr = cos(roll), sr = sin(roll);
    float cp = cos(pitch), sp = sin(pitch);
    mat3 ry = mat3(cy, 0.0, -sy, 0.0, 1.0, 0.0, sy, 0.0, cy);
    mat3 rz = mat3(cr, sr, 0.0, -sr, cr, 0.0, 0.0, 0.0, 1.0);
    mat3 rx = mat3(1.0, 0.0, 0.0, 0.0, cp, sp, 0.0, -sp, cp);
    return ry * rx * rz;
}

void main()
{
    float t = sceneTime();
    mat3 attitude = shipAttitude(t);
    vec3 p = attitude * inPos.xyz + vec3(0.0, 0.06 * sin(t * 0.8), 0.0);
    vec4 world = ubo.model * vec4(p, 1.0);
    worldPos = world.xyz;
    worldNormal = mat3(ubo.model) * (attitude * inNormal);
    partUV = inUV;
    objectPos = inPos.xyz;
    gl_Position = ubo.projection * ubo.view * world;
}
