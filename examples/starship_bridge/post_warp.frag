#version 450
#extension GL_GOOGLE_include_directive : enable

#define ACCENT vec3(1.0, 0.45, 0.18)
#define PANE_LABEL_ID 3
#define HAS_OVERLAY
#include "default_parameters.h"
#include "common.glsl"

// Output gauge: a vertical bar graph on the right that tracks the core pulse.
vec3 paneOverlay(vec2 p, float aspect, float t)
{
    vec2 q = vec2(p.x * aspect, 1.0 - p.y);
    vec2 g = (q - vec2(aspect - 0.12, 0.12)) / vec2(0.035, 0.6);
    if (g.x < 0.0 || g.x > 1.0 || g.y < 0.0 || g.y > 1.0)
        return vec3(0.0);
    float level = 0.62 + 0.25 * sin(t * 1.3) + 0.08 * sin(t * 5.0);
    float cell = fract(g.y * 24.0);
    float on = step(g.y, level) * step(0.25, cell);
    vec3 c = mix(vec3(0.2, 0.6, 1.0), vec3(1.0, 0.4, 0.15), smoothstep(0.6, 0.95, g.y));
    return c * on * 0.7 + vec3(1.0, 0.45, 0.18) * step(0.25, cell) * 0.06;
}

#include "post_common.glsl"
