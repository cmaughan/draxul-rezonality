#version 450
#extension GL_GOOGLE_include_directive : enable

#define EXPOSURE 0.8
#define BLOOM_TINT vec3(1.0, 0.7, 0.5)
#define BLOOM_GAIN 0.6
#include "post_common.glsl"
