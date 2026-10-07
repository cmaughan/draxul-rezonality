#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"

// Full-screen triangle pair that also emits a world-space camera ray, so
// raymarched panes follow Rezonality's orbit/dolly camera controls.
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec3 ray;

void main()
{
    vec2 clip = inPos.xy;
    vec4 view = ubo.projectionInverse * vec4(clip, 1.0, 1.0);
    view = vec4(view.xy, -1.0, 0.0);
    ray = (ubo.viewInverse * view).xyz;
    uv = inUV;
    gl_Position = vec4(clip, 0.9999, 1.0);
}
