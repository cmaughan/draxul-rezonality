#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "kelp_common.glsl"

// MIDDAY seabed: rippled sand, rocky mounds with coralline crusts and
// urchins, all dancing with caustics.
layout(location = 0) in vec3 worldPos;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 linearDepth;

void main()
{
    float t = sceneTime();
    vec3 p = worldPos;
    vec3 cam = cameraOrigin();
    float dist = length(cam - p);
    vec3 rd = (p - cam) / dist;

    vec2 e = vec2(0.05, 0.0);
    float h = seabedHeight(p.xz);
    vec3 n = normalize(vec3(h - seabedHeight(p.xz + e.xy), e.x, h - seabedHeight(p.xz + e.yx)));

    // Sand ripples perpendicular to the surge.
    float ripplePhase = dot(p.xz, vec2(0.8, 0.6)) * 3.2 + fbm2(p.xz * 0.4, 3) * 4.0;
    float ripple = sin(ripplePhase);
    float fade = 1.0 / (1.0 + dist * 0.08);
    n = normalize(n + vec3(0.8, 0.0, 0.6) * cos(ripplePhase) * 0.25 * fade);

    float rock = smoothstep(0.9, 2.0, h - 0.35 - 0.45 * sin(p.x * 0.09));
    vec3 sand = mix(vec3(0.72, 0.64, 0.48), vec3(0.85, 0.78, 0.6), 0.5 + 0.5 * ripple) * (0.85 + 0.3 * fbm2(p.xz * 2.0, 3));
    float pebble = smoothstep(0.12, 0.05, voronoi2(p.xz * 3.0)) * step(0.7, hash12(floor(p.xz * 3.0)));
    sand = mix(sand, vec3(0.35, 0.33, 0.3), pebble * 0.6);
    vec3 stone = mix(vec3(0.24, 0.22, 0.2), vec3(0.6, 0.3, 0.36), smoothstep(0.5, 0.75, fbm2(p.xz * 0.9, 4)));
    // Purple urchins tucked into the rock.
    float urchinCell = voronoi2(p.xz * 1.1 + 7.0);
    float urchin = smoothstep(0.14, 0.08, urchinCell) * step(0.6, hash12(floor(p.xz * 1.1 + 7.0))) * rock;
    stone = mix(stone, vec3(0.12, 0.03, 0.12), urchin);
    vec3 albedo = mix(sand, stone, rock);

    float ndl = max(dot(n, -LIGHT_DIR), 0.0);
    float caus = causticAt(p, t);
    vec3 light = SUN_UNDER * ndl * (0.2 + 1.3 * caus) + waterColor(n) * 0.7;
    vec3 col = albedo * light;

    col = underwater(col, p, rd, dist);
    fragColor = vec4(col, 1.0);
    linearDepth = vec4(dist);
}
