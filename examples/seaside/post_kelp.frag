#version 450
#extension GL_GOOGLE_include_directive : enable

#define EXPOSURE 1.0
#define BLOOM_TINT vec3(0.7, 1.0, 0.9)
#define BLOOM_GAIN 0.5
#define HAS_GRADE
#include "default_parameters.h"
#include "common.glsl"

// Deepen shadows toward blue and add a touch of contrast.
vec3 paneGrade(vec3 col, vec2 p, float t)
{
    float l = dot(col, vec3(0.3, 0.55, 0.15));
    col = mix(col * vec3(0.8, 0.9, 1.1), col, smoothstep(0.05, 0.4, l));
    return pow(col, vec3(1.12));
}

#include "post_common.glsl"
