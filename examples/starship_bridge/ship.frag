#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"

layout(location = 0) in vec3 worldPos;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 partUV;
layout(location = 3) in vec3 objectPos;
layout(location = 0) out vec4 fragColor;

const vec3 SUN_DIR = normalize(vec3(0.75, 0.55, 0.1));

// UV.x part ids written by generate_ship.py.
const float PART_ENGINE = 0.30;
const float PART_PLASMA = 0.55;
const float PART_WINDOW = 0.80;

bool isPart(float id)
{
    return abs(partUV.x - id) < 0.05;
}

float panelLines(vec3 p)
{
    // Hull plating seams laid out in object space.
    vec3 q = p * vec3(14.0, 20.0, 9.0);
    q.x += floor(q.z) * 0.37;
    vec3 f = abs(fract(q) - 0.5);
    vec3 w = fwidth(q) * 1.5;
    float seam = max(smoothstep(w.x, 0.0, 0.5 - f.x), smoothstep(w.z, 0.0, 0.5 - f.z));
    return seam;
}

void main()
{
    float t = sceneTime();
    vec3 n = normalize(worldNormal);
    vec3 v = normalize(cameraOrigin() - worldPos);
    if (dot(n, v) < 0.0)
        n = -n;

    if (isPart(PART_ENGINE))
    {
        float flicker = 0.9 + 0.1 * sin(t * 37.0 + objectPos.x * 9.0);
        float core = 1.0 - smoothstep(0.0, 1.0, partUV.y);
        vec3 hot = mix(vec3(0.15, 0.45, 1.0), vec3(0.85, 0.95, 1.0), core);
        fragColor = vec4(hot * (6.0 + 6.0 * core) * flicker, 1.0);
        return;
    }
    if (isPart(PART_PLASMA))
    {
        float flow = 0.5 + 0.5 * sin(partUV.y * 40.0 - t * 9.0);
        vec3 plasma = mix(vec3(0.05, 0.5, 1.0), vec3(0.5, 0.95, 1.0), flow);
        fragColor = vec4(plasma * (2.2 + 2.0 * flow), 1.0);
        return;
    }
    if (isPart(PART_WINDOW))
    {
        vec2 w = vec2(atan(objectPos.x, objectPos.y - 0.2) * 9.0, objectPos.z * 60.0);
        vec2 f = fract(w);
        float lit = step(0.3, hash12(floor(w)));
        float window = step(0.2, f.x) * step(f.x, 0.8) * step(0.25, f.y) * step(f.y, 0.75) * lit;
        vec3 base = vec3(0.04, 0.05, 0.07);
        fragColor = vec4(base + vec3(1.0, 0.8, 0.5) * window * 3.0, 1.0);
        return;
    }

    // Hull: brushed gunmetal with panel seams, key/fill/rim lighting and
    // reflections of the nebula.
    float seams = panelLines(objectPos);
    float plateVariation = hash12(floor(objectPos.xz * vec2(9.0, 5.5)) + floor(objectPos.y * 14.0));
    float grime = fbm3(objectPos * 6.0, 3);
    vec3 albedo = mix(vec3(0.13, 0.15, 0.18), vec3(0.22, 0.24, 0.27), plateVariation);
    albedo *= 0.75 + 0.5 * grime;
    albedo *= 1.0 - seams * 0.35;

    float ndl = max(dot(n, SUN_DIR), 0.0);
    vec3 h = normalize(SUN_DIR + v);
    float spec = pow(max(dot(n, h), 0.0), 64.0) * (0.6 + 0.4 * plateVariation);
    float fres = pow(1.0 - max(dot(n, v), 0.0), 4.0);

    vec3 key = vec3(1.0, 0.82, 0.62) * 1.2;
    vec3 fill = vec3(0.12, 0.25, 0.55);
    vec3 col = albedo * (key * ndl + fill * (0.5 + 0.5 * n.y));
    col += key * spec * 0.5;

    vec3 refl = reflect(-v, n);
    col += nebula(refl, vec3(0.05, 0.22, 0.55), vec3(0.55, 0.08, 0.38), t) * 0.35 * (0.3 + fres);
    col += vec3(0.25, 0.75, 1.0) * fres * 0.8;

    // Tactical scan sweeping stern-to-bow, tracing a holographic wireframe.
    float sweep = fract(t * 0.18) * 5.6 - 2.8;
    float band = exp(-abs(objectPos.z - sweep) * 9.0);
    col += vec3(0.2, 0.9, 1.0) * (band * 0.25 + band * seams * 2.5);

    // Navigation lights: port red, starboard green, blinking white on the spine.
    float blink = step(0.85, fract(t * 0.7));
    vec3 nav = vec3(0.0);
    nav += vec3(3.0, 0.1, 0.05) * exp(-length(objectPos - vec3(-1.18, -0.28, 0.9)) * 30.0);
    nav += vec3(0.1, 3.0, 0.3) * exp(-length(objectPos - vec3(1.18, -0.28, 0.9)) * 30.0);
    nav += vec3(4.0) * blink * exp(-length(objectPos - vec3(0.0, 0.27, 0.2)) * 35.0);
    col += nav * 4.0;

    fragColor = vec4(col, 1.0);
}
