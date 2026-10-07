#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "kelp_common.glsl"

// One baked mesh holds every fish; each finds its place from its id.
// color = (id, along-body 0..1, species 0 sardine / 1 garibaldi).
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec3 worldPos;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec3 fishData;
layout(location = 3) out vec3 bodyPos;

void main()
{
    float t = sceneTime();
    float id = inColor.r;
    float along = inColor.g;
    float species = inColor.b;
    vec3 rnd = hash33(vec3(id * 917.0, id * 131.0, id * 57.0));

    vec3 local = inPos.xyz;
    vec3 n = inNormal;
    vec3 pos;
    vec3 fwd;
    float size;
    float beat;
    if (species < 0.5)
    {
        // Sardines stream around the loop as a ribbon-shaped school.
        size = 0.5 + 0.12 * rnd.x;
        float s = t * 0.22 + rnd.y * 2.2;
        vec3 c = schoolPath(s);
        vec3 c1 = schoolPath(s + 0.01);
        fwd = normalize(c1 - c);
        vec3 side = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)));
        vec3 up = cross(side, fwd);
        float ang = rnd.z * TAU + t * 0.3;
        float rad = sqrt(fract(rnd.x * 7.31)) * (2.0 + 0.9 * sin(s * 3.0));
        pos = c + side * cos(ang) * rad * 1.4 + up * sin(ang) * rad * 0.6;
        // Small individual jitter in heading.
        fwd = normalize(fwd + side * 0.12 * sin(t * 1.7 + rnd.x * 30.0));
        beat = t * 13.0;
    }
    else
    {
        // Garibaldi hang near the rocks, turning lazily.
        size = 0.55 + 0.12 * rnd.x;
        vec3 anchor = vec3(-9.0 + 18.0 * rnd.y, 0.0, -18.0 + 14.0 * rnd.z);
        anchor.y = seabedHeight(anchor.xz) + 1.2 + 1.5 * rnd.x;
        float a = t * (0.18 + 0.1 * rnd.x) + rnd.z * TAU;
        pos = anchor + vec3(cos(a) * 1.6, 0.35 * sin(a * 1.7), sin(a) * 1.0);
        fwd = normalize(vec3(-sin(a) * 1.6, 0.2 * cos(a * 1.7), cos(a) * 1.0));
        beat = t * 5.0;
    }

    // Tail beat: a travelling wave that grows toward the tail.
    float wave = sin(beat + rnd.x * 40.0 - along * 6.0) * 0.12 * along * along;
    local.x += wave;
    n.x -= cos(beat + rnd.x * 40.0 - along * 6.0) * 0.3 * along;

    vec3 side = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)));
    vec3 up = cross(side, fwd);
    mat3 orient = mat3(side, up, -fwd); // model forward is -Z
    vec3 world = pos + orient * (local * size);

    worldPos = world;
    worldNormal = orient * n;
    fishData = inColor;
    bodyPos = inPos.xyz;
    gl_Position = ubo.projection * ubo.view * vec4(world, 1.0);
}
