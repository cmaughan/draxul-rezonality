#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "kelp_common.glsl"

// Golden-olive kelp that glows where sunlight shines through the blades.
layout(location = 0) in vec3 worldPos;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 partUV;
layout(location = 3) in vec3 kelpData;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 linearDepth;

const float STIPE = 0.1;
const float BLADE = 0.5;
const float BLADDER = 0.8;

void main()
{
    float t = sceneTime();
    vec3 p = worldPos;
    vec3 cam = cameraOrigin();
    float dist = length(cam - p);
    vec3 v = (cam - p) / dist;
    vec3 n = normalize(worldNormal);
    if (dot(n, v) < 0.0)
        n = -n;

    float part = partUV.x;
    vec3 albedo;
    float translucency;
    if (abs(part - BLADE) < 0.05)
    {
        float u = kelpData.b;
        // Corrugated blade with a darker midrib and lighter tips.
        float vein = smoothstep(0.08, 0.0, abs(fract(u * 9.0 + kelpData.r * 3.0) - 0.5) - 0.42);
        albedo = mix(vec3(0.32, 0.24, 0.06), vec3(0.55, 0.45, 0.12), u) * (1.0 - 0.25 * vein);
        translucency = 1.0;
    }
    else if (abs(part - BLADDER) < 0.05)
    {
        albedo = vec3(0.5, 0.38, 0.1);
        translucency = 0.7;
    }
    else
    {
        albedo = vec3(0.22, 0.16, 0.05);
        translucency = 0.2;
    }

    float ndl = max(dot(n, -LIGHT_DIR), 0.0);
    float caus = causticAt(p, t);
    vec3 col = albedo * (SUN_UNDER * ndl * (0.3 + 1.2 * caus) + waterColor(n) * 0.8);
    // Transmission: light from above shining through toward the viewer.
    float through = max(dot(n, LIGHT_DIR), 0.0) + 0.35 * max(dot(-v, -LIGHT_DIR), 0.0);
    col += vec3(0.95, 0.7, 0.18) * SUN_UNDER * through * translucency * 0.55 * (0.6 + 0.8 * caus);
    // Sheen on wet blades.
    vec3 h = normalize(-LIGHT_DIR + v);
    col += SUN_UNDER * pow(max(dot(n, h), 0.0), 40.0) * 0.15;

    col = underwater(col, p, -v, dist);
    fragColor = vec4(col, 1.0);
    linearDepth = vec4(dist);
}
