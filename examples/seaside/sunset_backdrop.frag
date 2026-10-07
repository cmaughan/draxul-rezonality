#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "sunset_sky.glsl"

// SUNSET backdrop: sky, islands, gulls and the far sea beyond the ocean mesh.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 ray;
layout(location = 0) out vec4 fragColor;

vec3 gullFlock(vec3 ro, vec3 rd, float t)
{
    float cover = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        float fi = float(i);
        float phase = t * (0.05 + 0.015 * hash11(fi + 3.0)) + fi * 2.3;
        vec3 c = vec3(-40.0 + 22.0 * sin(phase) + fi * 9.0, 16.0 + 4.0 * sin(phase * 1.3 + fi) + fi * 2.0,
            -70.0 - 12.0 * cos(phase) - fi * 6.0);
        vec3 toC = c - ro;
        vec3 fwd = normalize(toC);
        vec3 right = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)));
        vec3 up = cross(right, fwd);
        float along = dot(rd, fwd);
        if (along <= 0.0)
            continue;
        vec3 hit = ro + rd * (length(toC) / along);
        vec2 q = vec2(dot(hit - c, right), dot(hit - c, up)) / 1.3;
        float flap = sin(t * (3.5 + fi * 0.7) + fi * 2.0);
        flap = mix(0.55, flap, step(0.6, fract(t * 0.1 + fi * 0.27)));
        cover = max(cover, gullShape(rot2(0.15 * sin(phase * 2.0)) * q, flap));
    }
    return vec3(cover);
}

void main()
{
    float t = sceneTime();
    vec3 ro = cameraOrigin();
    vec3 rd = normalize(ray);
    vec3 col;
    if (rd.y >= -0.0005)
    {
        col = sunsetSky(rd, t);
        vec3 islandCol;
        float isl = islands(rd, islandCol);
        col = mix(col, islandCol, isl);
        col = mix(col, vec3(0.12, 0.05, 0.08), gullFlock(ro, rd, t).x * 0.92);
    }
    else
    {
        // Far sea: a calm mirror that the haze swallows at the horizon.
        float dist = ro.y / max(-rd.y, 1e-4);
        vec3 r = reflect(rd, vec3(0.0, 1.0, 0.0));
        float fres = 0.02 + 0.98 * pow(1.0 - max(-rd.y, 0.0), 5.0);
        col = mix(vec3(0.02, 0.03, 0.05), sunsetSkyNoClouds(r), fres);
        col = seaHaze(col, rd, dist);
    }
    fragColor = vec4(col, 1.0);
}
