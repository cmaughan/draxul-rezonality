#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "sunset_sky.glsl"

// SUNSET yacht: navy topsides with a boot stripe, teak deck, cream sails that
// glow with transmitted sunlight and warm cabin ports.
layout(location = 0) in vec3 worldPos;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 partUV;
layout(location = 3) in vec3 objectPos;
layout(location = 4) in vec3 sailUV;
layout(location = 0) out vec4 fragColor;

const float HULL = 0.05;
const float DECK = 0.15;
const float CABIN = 0.25;
const float SPAR = 0.35;
const float MAINSAIL = 0.5;
const float JIB = 0.6;
const float PENNANT = 0.7;
const float WINDOW = 0.8;
const float STAY = 0.9;

bool isPart(float id)
{
    return abs(partUV.x - id) < 0.04;
}

vec3 lightSurface(vec3 albedo, vec3 n, vec3 v, float gloss, float specAmount)
{
    float ndl = max(dot(n, SUN_DIR), 0.0);
    vec3 skyAmb = mix(vec3(0.16, 0.1, 0.18), vec3(0.3, 0.16, 0.2), n.y * 0.5 + 0.5);
    vec3 bounce = vec3(0.25, 0.12, 0.08) * max(-n.y, 0.0);
    vec3 col = albedo * (SUN_COL * 0.16 * ndl + skyAmb + bounce);
    vec3 h = normalize(SUN_DIR + v);
    col += SUN_COL * specAmount * pow(max(dot(n, h), 0.0), gloss) * 0.12;
    float fres = pow(1.0 - max(dot(n, v), 0.0), 5.0);
    col += sunsetSkyNoClouds(reflect(-v, n)) * fres * specAmount * 0.6;
    return col;
}

void main()
{
    float t = sceneTime();
    vec3 cam = cameraOrigin();
    vec3 v = normalize(cam - worldPos);
    vec3 n = normalize(worldNormal);
    bool backFace = dot(n, v) < 0.0;
    if (backFace)
        n = -n;
    vec3 col;

    if (isPart(MAINSAIL) || isPart(JIB))
    {
        float vtx = sailUV.g;
        // Cross-cut panel seams and a few battens on the main.
        float seams = smoothstep(0.03, 0.0, 0.5 - abs(fract(vtx * 11.0) - 0.5));
        float chord = sailUV.r / max(1.0 - vtx, 1e-3);
        float battens = isPart(MAINSAIL) ? smoothstep(0.05, 0.0, 0.5 - abs(fract(vtx * 4.0 + 0.5) - 0.5)) * step(0.6, chord) : 0.0;
        vec3 cloth = vec3(0.92, 0.86, 0.74) * (1.0 - 0.18 * seams) * (1.0 - 0.2 * battens);
        vec3 reflected = lightSurface(cloth, n, v, 12.0, 0.15);
        // Sun behind the sail: light shines through the cloth.
        float through = max(dot(-n, SUN_DIR), 0.0);
        vec3 transmitted = SUN_COL * vec3(1.0, 0.75, 0.5) * cloth * (0.05 + 0.3 * through * through) * through;
        transmitted *= (1.0 - 0.45 * seams) * (1.0 - 0.5 * battens);
        col = reflected + transmitted;
    }
    else if (isPart(HULL))
    {
        float y = objectPos.y;
        vec3 albedo = vec3(0.025, 0.05, 0.12);
        float boot = smoothstep(0.012, 0.0, abs(y - 0.16) - 0.045);
        albedo = mix(albedo, vec3(0.85, 0.82, 0.76), boot);
        albedo = mix(albedo, vec3(0.32, 0.05, 0.04), step(y, 0.1));
        // Sheer-line cove stripe in gold leaf.
        float cove = smoothstep(0.01, 0.0, abs(y - (0.52 + 0.3 * pow(max(-objectPos.z - 1.0, 0.0) / 3.2, 3.0))) - 0.012);
        albedo = mix(albedo, vec3(0.9, 0.6, 0.2), cove);
        col = lightSurface(albedo, n, v, 90.0, 1.0);
    }
    else if (isPart(DECK))
    {
        float plank = fract(objectPos.x * 7.0);
        float seam = smoothstep(0.08, 0.0, plank) + smoothstep(0.92, 1.0, plank);
        float grain = fbm2(vec2(objectPos.x * 40.0, objectPos.z * 2.0), 3);
        vec3 teak = mix(vec3(0.42, 0.25, 0.12), vec3(0.6, 0.38, 0.2), grain);
        teak = mix(teak, vec3(0.05, 0.03, 0.02), seam * 0.8);
        col = lightSurface(teak, n, v, 30.0, 0.2);
    }
    else if (isPart(CABIN))
        col = lightSurface(vec3(0.8, 0.76, 0.68), n, v, 40.0, 0.4);
    else if (isPart(SPAR))
        col = lightSurface(vec3(0.55, 0.36, 0.18), n, v, 60.0, 0.7);
    else if (isPart(WINDOW))
        col = vec3(1.0, 0.62, 0.28) * 3.0;
    else if (isPart(PENNANT))
    {
        float through = max(dot(-n, SUN_DIR), 0.0);
        col = lightSurface(vec3(0.7, 0.05, 0.04), n, v, 8.0, 0.1) + vec3(1.0, 0.15, 0.05) * through * 1.5;
    }
    else
        col = lightSurface(vec3(0.06), n, v, 20.0, 0.2);

    fragColor = vec4(col, 1.0);
}
