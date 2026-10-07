#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"

// Eighth-resolution wide blur of the bright pass, for the soft outer halo.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(set = 1, binding = 0) uniform sampler2D BloomA;

void main()
{
    vec2 texel = 1.0 / vec2(textureSize(BloomA, 0));
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int y = -3; y <= 3; ++y)
    {
        for (int x = -3; x <= 3; ++x)
        {
            float w = exp(-float(x * x + y * y) / 6.0);
            sum += texture(BloomA, uv + vec2(x, y) * texel * 1.5).rgb * w;
            total += w;
        }
    }
    fragColor = vec4(sum / total, 1.0);
}
