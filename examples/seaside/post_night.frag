#version 450
#extension GL_GOOGLE_include_directive : enable

#define EXPOSURE 1.1
#define BLOOM_TINT vec3(0.9, 0.9, 1.0)
#define BLOOM_GAIN 0.6
#define STREAK_GAIN 0.05
#define STREAK_TINT vec3(0.6, 0.75, 1.0)
#include "post_common.glsl"
