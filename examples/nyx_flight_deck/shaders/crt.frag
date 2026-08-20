#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(set = 1, binding = 0) uniform sampler2D Signal;

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main()
{
    vec2 outputSize = max(ubo.iResolution.xy, vec2(1.0));
    vec2 signalSize = max(vec2(textureSize(Signal, 0)), vec2(1.0));
    vec2 centered = uv * 2.0 - 1.0;
    vec2 curved = centered * (1.0 + 0.055 * dot(centered, centered));
    vec2 signalUv = curved * 0.5 + 0.5;

    float animatedTime = ubo.iTime * 2.0;
    float timeStep = floor(animatedTime * 30.0);
    float sourceLine = floor(signalUv.y * signalSize.y);
    // A glitch occupies two low-resolution rows, producing a deliberately
    // chunky horizontal tear instead of a single hairline disturbance.
    float glitchBlock = floor(sourceLine * 0.5);
    float lineNoise = hash12(vec2(glitchBlock, timeStep));
    float tear = smoothstep(0.93, 1.0, lineNoise);
    float rollingBand = exp(-abs(signalUv.y - fract(animatedTime * 0.071)) * 48.0);
    signalUv.x += (lineNoise - 0.5) * (0.70 + tear * 9.0) / signalSize.x;
    signalUv.x += rollingBand * 0.014;

    vec2 snappedUv = (floor(signalUv * signalSize) + 0.5) / signalSize;
    vec2 texel = 1.0 / signalSize;
    vec3 color;
    color.r = texture(Signal, snappedUv + vec2(texel.x * 0.84, 0.0)).r;
    color.g = texture(Signal, snappedUv).g;
    color.b = texture(Signal, snappedUv - vec2(texel.x * 0.84, 0.0)).b;

    float outputLine = floor(uv.y * outputSize.y);
    float scanline = mix(0.28, 1.0, 0.5 + 0.5 * cos(outputLine * 3.14159265));
    float phosphor = 0.93 + 0.07 * sin(uv.x * outputSize.x * 3.14159265);
    float grain = hash12(floor(uv * outputSize) + vec2(timeStep, timeStep * 0.37)) - 0.5;
    float dropout = step(0.995, hash12(vec2(floor(outputLine * 0.5) * 0.13, timeStep * 1.71)));
    color *= scanline * phosphor;
    color += grain * 0.095 + rollingBand * vec3(0.05, 0.11, 0.13);
    color = mix(color, color * vec3(0.20, 0.55, 0.62), dropout * 0.72);

    float vignette = smoothstep(1.22, 0.36, length(centered * vec2(0.82, 0.68)));
    float inside = step(0.0, signalUv.x) * step(signalUv.x, 1.0)
        * step(0.0, signalUv.y) * step(signalUv.y, 1.0);
    color *= vignette * inside;
    fragColor = vec4(max(color, vec3(0.0)), 1.0);
}
