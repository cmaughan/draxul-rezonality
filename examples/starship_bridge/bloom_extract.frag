#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"

// Quarter-resolution bright-pass: soft-threshold the HDR scene and gather a
// wide 13-tap kernel so the next blur stage starts from a smooth signal.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(set = 1, binding = 0) uniform sampler2D Scene;

vec3 bright(vec2 p)
{
    vec3 c = texture(Scene, p).rgb;
    float l = max(c.r, max(c.g, c.b));
    float knee = 0.55;
    float w = clamp((l - 0.6 + knee) / (2.0 * knee), 0.0, 1.0);
    w = w * w * (l > 0.6 ? 1.0 : 0.6);
    return c * w;
}

void main()
{
    vec2 texel = 1.0 / vec2(textureSize(Scene, 0));
    vec3 sum = bright(uv) * 0.125;
    vec2 o1 = texel * 2.0;
    vec2 o2 = texel * 4.0;
    sum += (bright(uv + vec2(-o1.x, -o1.y)) + bright(uv + vec2(o1.x, -o1.y))
        + bright(uv + vec2(-o1.x, o1.y)) + bright(uv + vec2(o1.x, o1.y))) * 0.125;
    sum += (bright(uv + vec2(-o2.x, 0)) + bright(uv + vec2(o2.x, 0))
        + bright(uv + vec2(0, -o2.y)) + bright(uv + vec2(0, o2.y))) * 0.0625;
    sum += (bright(uv + vec2(-o2.x, -o2.y)) + bright(uv + vec2(o2.x, -o2.y))
        + bright(uv + vec2(-o2.x, o2.y)) + bright(uv + vec2(o2.x, o2.y))) * 0.03125;
    fragColor = vec4(sum, 1.0);
}
