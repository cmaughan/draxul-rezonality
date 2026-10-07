#version 450
#extension GL_GOOGLE_include_directive : enable

#define ACCENT vec3(0.3, 1.0, 0.6)
#define PANE_LABEL_ID 4
#define HAS_OVERLAY
#include "default_parameters.h"
#include "common.glsl"

// Mini radar scope in the lower-right corner.
vec3 paneOverlay(vec2 p, float aspect, float t)
{
    vec2 q = vec2(p.x * aspect, 1.0 - p.y);
    vec2 c = vec2(aspect - 0.2, 0.2);
    vec2 d = q - c;
    float r = length(d) / 0.12;
    if (r > 1.05)
        return vec3(0.0);
    float px = 1.5 / (ubo.iResolution.y * 0.12);
    float a = atan(d.y, d.x);
    float sweep = mod(a - t * 1.6, TAU);
    float trail = exp(-sweep * 2.5) * step(r, 1.0);
    float rings = smoothstep(px, 0.0, abs(r - 1.0)) + smoothstep(px, 0.0, abs(r - 0.5)) * 0.5;
    float cross = (smoothstep(px, 0.0, abs(d.x / 0.12)) + smoothstep(px, 0.0, abs(d.y / 0.12))) * step(r, 1.0) * 0.3;
    vec3 col = vec3(0.3, 1.0, 0.6) * (rings * 0.6 + cross + trail * 0.35);
    for (int i = 0; i < 5; ++i)
    {
        vec2 b = vec2(cos(float(i) * 2.3 + t * 0.05), sin(float(i) * 1.7)) * (0.3 + 0.12 * float(i));
        float bd = length(d / 0.12 - b);
        float ba = mod(atan(b.y, b.x) - t * 1.6, TAU);
        col += vec3(1.0, 0.7, 0.3) * exp(-bd * 40.0) * exp(-ba * 1.2) * 2.0;
    }
    return col * 0.8;
}

#include "post_common.glsl"
