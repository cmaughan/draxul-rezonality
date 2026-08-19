#version 450
#extension GL_GOOGLE_include_directive : enable
#include "default_parameters.h"
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() {
    vec2 p=uv-.5;
    p.x*=ubo.iResolution.x/ubo.iResolution.y;
    float ring=smoothstep(.018,.0,abs(length(p)-.28));
    color=vec4(mix(vec3(.025,.04,.09),vec3(.2,.8,1.),ring),1.);
}
