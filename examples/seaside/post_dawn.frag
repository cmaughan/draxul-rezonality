#version 450
#extension GL_GOOGLE_include_directive : enable

#define EXPOSURE 0.9
#define BLOOM_TINT vec3(1.0, 0.75, 0.6)
#define BLOOM_GAIN 0.55
#include "post_common.glsl"
