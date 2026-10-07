#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "night_common.glsl"

// NIGHT volumetrics: integrates the two sweeping beams through drifting sea
// mist up to the scene's linear depth, then adds the lantern's glare.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 ray;
layout(location = 0) out vec4 fragColor;
layout(set = 1, binding = 0) uniform sampler2D Scene;
layout(set = 1, binding = 1) uniform sampler2D Linear;

float mist(vec3 p, float t)
{
    return 0.35 + 1.1 * smoothstep(0.3, 0.8, fbm3(p * 0.07 + vec3(t * 0.25, 0.0, t * 0.08), 3));
}

vec3 beamScatter(vec3 ro, vec3 rd, float maxT, vec3 b, float t, float jitter)
{
    // Closest approach between the view ray and the beam axis.
    vec3 w0 = ro - LAMP_POS;
    float bb = dot(rd, b);
    float d = dot(rd, w0);
    float e = dot(b, w0);
    float denom = max(1.0 - bb * bb, 1e-4);
    float s = (bb * e - d) / denom;
    float u = max((e - bb * d) / denom, 0.0);
    float width = 0.5 + 0.05 * u;
    float half_ = min(3.5 * width / sqrt(denom), maxT);
    float s0 = clamp(s - half_, 0.0, maxT);
    float s1 = clamp(s + half_, 0.0, maxT);
    if (s1 <= s0)
        return vec3(0.0);

    // Henyey-Greenstein forward scattering toward the viewer.
    float g = 0.55;
    float cosT = dot(b, -rd);
    float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5) * 0.08;

    const int STEPS = 20;
    float stepLen = (s1 - s0) / float(STEPS);
    float acc = 0.0;
    for (int i = 0; i < STEPS; ++i)
    {
        vec3 p = ro + rd * (s0 + (float(i) + jitter) * stepLen);
        vec3 dl = p - LAMP_POS;
        float along = dot(dl, b);
        if (along <= 0.0)
            continue;
        float perp2 = max(dot(dl, dl) - along * along, 0.0);
        float wd = 0.5 + 0.05 * along;
        float profile = exp(-perp2 / (wd * wd));
        if (profile < 0.002)
            continue;
        float atten = 1.0 / (1.0 + along * 0.015);
        acc += profile * atten * mist(p, t) * stepLen;
    }
    return LAMP_COL * acc * phase;
}

void main()
{
    float t = sceneTime();
    vec3 ro = cameraOrigin();
    vec3 rd = normalize(ray);
    vec3 col = texture(Scene, uv).rgb;
    float depth = texture(Linear, uv).r;
    float maxT = min(depth, 600.0);
    float jitter = hash12(gl_FragCoord.xy + fract(t * 7.3) * 113.0);

    col += beamScatter(ro, rd, maxT, beamDir(t, 0.0), t, jitter);
    col += beamScatter(ro, rd, maxT, beamDir(t, 1.0), t, jitter);

    // Lantern glare, flaring as a beam swings through the viewer.
    vec3 toLamp = LAMP_POS - ro;
    float lampDist = length(toLamp);
    if (depth > lampDist - 1.6)
    {
        float ang = acos(clamp(dot(rd, toLamp / lampDist), -1.0, 1.0));
        vec3 toCam = -toLamp / lampDist;
        float facing = max(max(dot(beamDir(t, 0.0), toCam), dot(beamDir(t, 1.0), toCam)), 0.0);
        float flash = pow(facing, 24.0);
        col += LAMP_COL * (exp(-ang * 160.0) * 3.0 + exp(-ang * 30.0) * 0.25) * (0.35 + 9.0 * flash);
    }
    fragColor = vec4(col, 1.0);
}
