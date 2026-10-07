#version 450
#extension GL_GOOGLE_include_directive : enable

#define ACCENT vec3(0.25, 0.85, 1.0)
#define PANE_LABEL_ID 1
#define HAS_OVERLAY
#include "default_parameters.h"
#include "common.glsl"

// Centre targeting reticle that slowly rotates and breathes.
vec3 paneOverlay(vec2 p, float aspect, float t)
{
    vec2 q = (p - 0.5) * vec2(aspect, 1.0);
    float r = length(q);
    float a = atan(q.y, q.x);
    float px = 1.5 / ubo.iResolution.y;
    float breathe = 0.36 + 0.01 * sin(t * 2.0);
    float ring = smoothstep(px, 0.0, abs(r - breathe)) * step(0.5, fract((a + t * 0.2) / TAU * 12.0));
    float inner = smoothstep(px, 0.0, abs(r - 0.42)) * step(0.85, fract(a / TAU * 72.0));
    return vec3(0.25, 0.85, 1.0) * (ring * 0.35 + inner * 0.3);
}

#include "post_common.glsl"
