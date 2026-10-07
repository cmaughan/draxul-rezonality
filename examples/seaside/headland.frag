#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "night_common.glsl"
#include "night_lighting.glsl"

// NIGHT headland: wet basalt, lichen, and a grassy crown.
layout(location = 0) in vec3 worldPos;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 partUV;
layout(location = 3) in vec3 vertexColor;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 linearDepth;

void main()
{
    vec3 p = worldPos;
    vec3 cam = cameraOrigin();
    float dist = length(cam - p);
    vec3 v = (cam - p) / dist;
    vec3 n = normalize(worldNormal);

    // Per-pixel rock relief.
    vec2 e = vec2(0.06, 0.0);
    float b0 = fbm3(p * 1.3, 4);
    vec3 grad = vec3(fbm3((p + e.xyy) * 1.3, 4), fbm3((p + e.yxy) * 1.3, 4), fbm3((p + e.yyx) * 1.3, 4)) - b0;
    vec3 bump = grad / e.x;
    bump -= n * dot(bump, n);
    n = normalize(n - bump * 0.18);

    float grass = smoothstep(0.7, 0.9, n.y) * smoothstep(6.5, 7.6, p.y);
    float lichen = smoothstep(0.6, 0.8, fbm3(p * 0.7 + 3.0, 3)) * smoothstep(2.0, 4.0, p.y);
    float wet = smoothstep(1.4, 0.1, p.y);
    vec3 albedo = mix(vec3(0.12, 0.115, 0.11), vec3(0.24, 0.22, 0.2), b0);
    albedo = mix(albedo, vec3(0.32, 0.26, 0.14), lichen * 0.6);
    albedo = mix(albedo, vec3(0.06, 0.1, 0.04), grass);
    albedo *= 1.0 - 0.5 * wet;
    vec3 col = nightLight(p, albedo, n, v, mix(10.0, 200.0, wet), 0.04 + wet * 0.6);

    col = nightHaze(col, -v, dist);
    fragColor = vec4(col, 1.0);
    linearDepth = vec4(dist);
}
