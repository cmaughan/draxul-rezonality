#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "sunset_sky.glsl"
#include "waves.glsl"
#include "boat_motion.glsl"

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec3 worldPos;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec2 partUV;
layout(location = 3) out vec3 objectPos;
layout(location = 4) out vec3 sailUV;

const float MAINSAIL = 0.5;
const float JIB = 0.6;
const float PENNANT = 0.7;

mat3 rotX(float a)
{
    float c = cos(a), s = sin(a);
    return mat3(1.0, 0.0, 0.0, 0.0, c, s, 0.0, -s, c);
}

mat3 rotZ(float a)
{
    float c = cos(a), s = sin(a);
    return mat3(c, s, 0.0, -s, c, 0.0, 0.0, 0.0, 1.0);
}

void main()
{
    float t = sceneTime();
    vec3 p = inPos.xyz;
    vec3 n = inNormal;
    float part = inUV.x;

    // Sails fill with wind: belly deepest a third of the way back from the luff.
    if (abs(part - MAINSAIL) < 0.04 || abs(part - JIB) < 0.04)
    {
        float u = inColor.r;
        float v = inColor.g;
        float chord = clamp(u / max(1.0 - v, 1e-3), 0.0, 1.0);
        float depth = (abs(part - MAINSAIL) < 0.04 ? 0.55 : 0.4) * pow(1.0 - v, 0.7);
        float belly = sin(PI * pow(chord, 0.75)) * depth;
        // Leech flutter near the trailing edge.
        belly += 0.035 * smoothstep(0.75, 1.0, chord) * sin(t * 11.0 + v * 9.0);
        vec3 sn = normalize(n);
        if (sn.x > 0.0)
            sn = -sn; // billow toward port, downwind of the boom
        p += sn * belly;
    }
    if (abs(part - PENNANT) < 0.04)
    {
        float u = inColor.r;
        p.x += sin(t * 9.0 - u * 7.0) * 0.12 * u;
        p.y -= 0.08 * u * u;
        p.x -= 0.4 * u; // streams downwind
    }

    // Damped response to the swell: heave, pitch and roll from four samples,
    // plus a steady heel to port from the wind.
    vec2 c = vec2(0.0);
    float h0 = seaHeightAt(c, t);
    float hb = seaHeightAt(c + BOAT_FWD * 3.2, t);
    float hs = seaHeightAt(c - BOAT_FWD * 3.2, t);
    float hr = seaHeightAt(c + BOAT_RIGHT * 1.4, t);
    float hl = seaHeightAt(c - BOAT_RIGHT * 1.4, t);
    float pitch = atan(hb - hs, 6.4) * 0.8;
    float roll = atan(hr - hl, 2.8) * 0.7 - 0.16 + 0.03 * sin(t * 0.7);
    mat3 attitude = rotX(pitch) * rotZ(roll);
    p = attitude * p;
    n = attitude * n;

    // Model space (x starboard, y up, -z forward) to world.
    vec3 fwd = vec3(BOAT_FWD.x, 0.0, BOAT_FWD.y);
    vec3 right = vec3(BOAT_RIGHT.x, 0.0, BOAT_RIGHT.y);
    mat3 toWorld = mat3(right, vec3(0.0, 1.0, 0.0), -fwd);
    vec3 world = toWorld * p + vec3(0.0, h0 * 0.85 - 0.12, 0.0);

    worldPos = world;
    worldNormal = toWorld * n;
    partUV = inUV;
    objectPos = inPos.xyz;
    sailUV = inColor;
    gl_Position = ubo.projection * ubo.view * vec4(world, 1.0);
}
