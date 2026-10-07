#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "night_common.glsl"
#include "night_lighting.glsl"

// NIGHT lighthouse: banded tower, iron gallery, blazing lantern and the
// keeper's cottage with its windows lit.
layout(location = 0) in vec3 worldPos;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 partUV;
layout(location = 3) in vec3 vertexColor;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 linearDepth;

const float TOWER = 0.05;
const float GALLERY = 0.15;
const float LANTERN = 0.25;
const float ROOF = 0.35;
const float WINDOW = 0.45;
const float DOOR = 0.55;
const float HOUSE = 0.65;
const float HOUSE_ROOF = 0.75;
const float RAIL = 0.85;

bool isPart(float id)
{
    return abs(partUV.x - id) < 0.04;
}

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
    vec3 col;

    if (isPart(TOWER))
    {
        float band = step(0.5, fract((p.y - 8.85) / 3.25));
        vec3 albedo = mix(vec3(0.82, 0.8, 0.76), vec3(0.55, 0.06, 0.05), band);
        albedo = mix(albedo, vec3(0.3, 0.3, 0.3), step(p.y, 9.0));
        albedo *= 0.85 + 0.15 * fbm3(p * vec3(3.0, 0.6, 3.0), 3);
        col = nightLight(p, albedo, n, v, 30.0, 0.1);
        // The gallery shades the tower top from the lantern.
        col *= 1.0 - 0.5 * smoothstep(20.0, 21.8, p.y);
    }
    else if (isPart(LANTERN))
    {
        // Brightest where a beam faces the viewer.
        vec3 toCam = normalize(cam - LAMP_POS);
        float facing = max(max(dot(beamDir(t, 0.0), toCam), dot(beamDir(t, 1.0), toCam)), 0.0);
        float mullionGlow = 0.6 + 0.4 * smoothstep(0.0, 0.4, abs(fract(atan(p.z, p.x) / TAU * 10.0) - 0.5));
        col = LAMP_COL * (4.0 + 30.0 * pow(facing, 16.0)) * mullionGlow;
    }
    else if (isPart(WINDOW))
    {
        float flicker = 0.92 + 0.08 * sin(t * 7.0 + p.x * 3.0) * sin(t * 4.3 + p.z);
        col = vec3(1.0, 0.6, 0.25) * 2.4 * flicker;
    }
    else if (isPart(GALLERY) || isPart(RAIL))
        col = nightLight(p, vec3(0.05, 0.05, 0.05), n, v, 40.0, 0.5);
    else if (isPart(ROOF))
        col = nightLight(p, vec3(0.2, 0.04, 0.03), n, v, 60.0, 0.8);
    else if (isPart(DOOR))
        col = nightLight(p, vec3(0.12, 0.07, 0.04), n, v, 20.0, 0.1);
    else if (isPart(HOUSE))
        col = nightLight(p, vec3(0.75, 0.73, 0.68) * (0.85 + 0.15 * fbm3(p * 2.0, 2)), n, v, 20.0, 0.05);
    else
    {
        // Slate roof courses.
        float course = smoothstep(0.1, 0.0, fract(p.y * 4.5));
        col = nightLight(p, vec3(0.1, 0.11, 0.13) * (1.0 - 0.4 * course), n, v, 40.0, 0.4);
    }

    col = nightHaze(col, -v, dist);
    fragColor = vec4(col, 1.0);
    linearDepth = vec4(dist);
}
