#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "kelp_common.glsl"

// Countershaded silver sardines that flash as they turn, and bright garibaldi.
layout(location = 0) in vec3 worldPos;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec3 fishData;
layout(location = 3) in vec3 bodyPos;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 linearDepth;

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
    float species = fishData.b;
    float along = fishData.g;

    vec3 albedo;
    float metal;
    if (species < 0.5)
    {
        float back = smoothstep(-0.02, 0.06, bodyPos.y);
        albedo = mix(vec3(0.75, 0.8, 0.82), vec3(0.06, 0.14, 0.2), back);
        // A row of dark spots along the flank.
        float spots = smoothstep(0.02, 0.0, length(vec2(fract(along * 9.0) - 0.5, (bodyPos.y - 0.02) * 9.0)) - 0.12);
        albedo *= 1.0 - 0.5 * spots * (1.0 - back);
        metal = 1.0 - back * 0.7;
    }
    else
    {
        albedo = vec3(1.0, 0.32, 0.03);
        metal = 0.15;
    }
    // Eye.
    float eye = smoothstep(0.03, 0.02, length(vec2(bodyPos.z + 0.38, bodyPos.y - 0.02 * species)));
    albedo = mix(albedo, vec3(0.02), eye);

    float ndl = max(dot(n, -LIGHT_DIR), 0.0);
    float caus = causticAt(p, t);
    vec3 col = albedo * (SUN_UNDER * ndl * (0.35 + 1.0 * caus) + waterColor(n) * 0.9) * (1.0 - metal * 0.4);
    // Mirror-like flanks pick up the bright water above and flash.
    vec3 r = reflect(-v, n);
    vec3 env = waterColor(r) * 1.6 + SUN_UNDER * pow(max(dot(r, -LIGHT_DIR), 0.0), 24.0) * 1.2;
    float fres = 0.3 + 0.7 * pow(1.0 - max(dot(n, v), 0.0), 3.0);
    col += env * metal * fres * vec3(0.85, 0.95, 1.0);

    col = underwater(col, p, -v, dist);
    fragColor = vec4(col, 1.0);
    linearDepth = vec4(dist);
}
