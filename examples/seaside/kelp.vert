#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "kelp_common.glsl"

// Giant kelp swaying in the surge. color = (strand phase, height on stipe,
// position along blade); uv.x = part id.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec3 worldPos;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec2 partUV;
layout(location = 3) out vec3 kelpData;

const float BLADE = 0.5;

void main()
{
    float t = sceneTime();
    vec3 p = inPos.xyz;
    float phase = inColor.r;
    float h = inColor.g;
    float bladeU = inColor.b;

    // Surge: a slow back-and-forth with a lazy secondary wobble, growing up the stipe.
    float bend = pow(h, 1.6);
    vec2 surge = vec2(0.85, 0.5) * (sin(t * 0.45 + phase * TAU + h * 1.2) * 1.6 + 0.9);
    surge += vec2(-0.4, 0.9) * sin(t * 0.8 + phase * 17.0 + h * 2.5) * 0.6;
    p.xz += surge * bend;
    p.y -= dot(surge, surge) * bend * 0.03;

    vec3 n = inNormal;
    if (abs(inUV.x - BLADE) < 0.05)
    {
        float flutter = sin(t * 2.2 + bladeU * 5.0 + phase * 40.0 + h * 9.0);
        p += normalize(n) * flutter * 0.12 * bladeU;
        p.xz += surge * 0.08 * bladeU;
    }

    worldPos = p;
    worldNormal = n;
    partUV = inUV;
    kelpData = inColor;
    gl_Position = ubo.projection * ubo.view * vec4(p, 1.0);
}
