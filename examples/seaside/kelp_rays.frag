#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "kelp_common.glsl"

// MIDDAY volumetrics: god rays falling through the water column up to the
// scene depth, drifting marine snow and a stream of rising bubbles.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 ray;
layout(location = 0) out vec4 fragColor;
layout(set = 1, binding = 0) uniform sampler2D Scene;
layout(set = 1, binding = 1) uniform sampler2D Linear;

float shaftPattern(vec3 p, float t)
{
    vec3 s = p - LIGHT_DIR * ((SURFACE_Y - p.y) / -LIGHT_DIR.y);
    vec2 q = s.xz * 0.16 + vec2(t * 0.035, t * 0.022);
    float f = fbm2(q, 4);
    return smoothstep(0.52, 0.8, f) * (0.55 + 0.45 * sin(q.x * 9.0 + f * 6.0));
}

void main()
{
    float t = sceneTime();
    vec3 ro = cameraOrigin();
    vec3 rd = normalize(ray);
    vec3 col = texture(Scene, uv).rgb;
    float depth = texture(Linear, uv).r;
    float maxT = min(depth, 70.0);
    if (rd.y > 0.0)
        maxT = min(maxT, (SURFACE_Y - ro.y) / rd.y);
    float jitter = hash12(gl_FragCoord.xy + fract(t * 3.1) * 71.0);

    // God rays.
    const int STEPS = 28;
    float stepLen = maxT / float(STEPS);
    float acc = 0.0;
    for (int i = 0; i < STEPS; ++i)
    {
        float s = (float(i) + jitter) * stepLen;
        vec3 p = ro + rd * s;
        float fromSurface = max(SURFACE_Y - p.y, 0.0);
        acc += shaftPattern(p, t) * exp(-fromSurface * 0.05) * exp(-s * 0.035) * stepLen;
    }
    float g = 0.6;
    float cosT = dot(LIGHT_DIR, -rd);
    float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5);
    col += vec3(0.5, 0.88, 0.72) * acc * (0.004 + 0.11 * phase);

    // Marine snow on a few depth layers.
    for (int k = 0; k < 6; ++k)
    {
        float d = 1.5 + float(k) * float(k) * 0.9;
        if (d > depth)
            break;
        vec3 p = ro + rd * d;
        vec3 q = p * 2.2 + vec3(0.0, t * 0.12, t * 0.05) + float(k) * 13.7;
        vec3 cell = floor(q);
        vec3 h = hash33(cell);
        vec3 c = cell + 0.2 + 0.6 * h;
        float r = length((q - c).xy);
        float size = 0.025 + 0.012 * float(k);
        float speck = smoothstep(size, 0.0, r) * step(0.75, h.z);
        col += vec3(0.6, 0.75, 0.7) * speck * 0.3 * exp(-d * 0.06);
    }

    // Bubbles rising from a vent among the rocks.
    vec3 vent = vec3(5.0, 0.0, -9.0);
    vent.y = seabedHeight(vent.xz);
    for (int b = 0; b < 18; ++b)
    {
        float fb = float(b);
        float life = fract(t * 0.11 + fb * 0.0557);
        vec3 c = vent + vec3(sin(life * 9.0 + fb) * 0.25 * life * 4.0, life * (SURFACE_Y - vent.y), cos(life * 7.0 + fb) * 0.25 * life * 4.0);
        float radius = 0.04 + 0.1 * hash11(fb * 3.7) + 0.06 * life;
        vec3 oc = ro - c;
        float bq = dot(oc, rd);
        float cq = dot(oc, oc) - radius * radius;
        float disc = bq * bq - cq;
        if (disc > 0.0)
        {
            float th = -bq - sqrt(disc);
            if (th > 0.0 && th < depth)
            {
                vec3 nrm = normalize(ro + rd * th - c);
                float rim = pow(1.0 - abs(dot(nrm, rd)), 2.0);
                float glint = pow(max(dot(reflect(rd, nrm), -LIGHT_DIR), 0.0), 30.0);
                col = mix(col, vec3(0.7, 0.95, 1.0) * (0.6 + rim) + SUN_UNDER * glint, rim * 0.7 + glint * 0.8);
            }
        }
    }
    fragColor = vec4(col, 1.0);
}
