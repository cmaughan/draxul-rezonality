#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"

layout(location = 0) out vec4 fragColor;
layout(set = 1, binding = 0) uniform sampler2D AudioAnalysis;

#define PI 3.1415926

vec2 distort(vec2 p, float power)
{
    float a = atan(p.y, p.x);
    float r = pow(length(p), power);
    return vec2((a / PI), r * 2.0 - 1.0);
}

// VkLive's stereo LED spectrum visualizer. The four texture rows are left
// FFT, right FFT, left waveform, and right waveform.
void main()
{
    vec2 uv = gl_FragCoord.xy / ubo.iResolution.xy;
    uv.y = 1.0 - uv.y;

    float bass = texture(AudioAnalysis, vec2(0, 0)).x * 12;
    const float bands = 100;
    const float segs = 100;
    vec2 p;
    p.x = floor(uv.x * bands) / bands;
    p.y = floor(uv.y * segs) / segs;

    float fft1 = texture(AudioAnalysis, vec2(p.x, 0.26)).x;
    float fft2 = texture(AudioAnalysis, vec2(p.x, 0.0)).x;

    vec3 color1 = mix(vec3(0.0, 2.0, 0.0),
        vec3(2.0, 0.0, 0.0), sqrt(max(uv.y * 2.0, 0.0)));
    vec3 color2 = mix(vec3(0.0, 2.0, 0.0),
        vec3(2.0, 0.0, 0.0),
        sqrt(max((uv.y - 0.5) * 2.0, 0.0)));
    color1 = clamp(color1, 0, 1);
    color2 = clamp(color2, 0, 1);

    fft1 = min(fft1, 1.0);
    fft2 = min(fft2, 1.0);
    float mask1 = p.y < (fft1 * 0.5) ? 1.0 : 0.05;
    float mask2 = p.y - 0.5 < fft2 * 0.5 ? 1.0 : 0.05;

    vec2 d = fract((uv - p) * vec2(bands, segs)) - 0.5;
    float led = smoothstep(0.5, 0.35, abs(d.x))
        * smoothstep(0.5, 0.35, abs(d.y));
    vec3 ledColor = led * color1 * mask1;
    ledColor += led * color2 * mask2;

    float wave = texture(AudioAnalysis, vec2(uv.x, 0.75)).x;
    vec3 waveColor = vec3(0.001, 0.01, 0.04)
        / abs(wave - uv.y + 0.66);
    float wave2 = texture(AudioAnalysis, vec2(uv.x, 0.55)).x;
    vec3 waveColor2 = vec3(0.04, 0.01, 0.001)
        / abs(wave2 - uv.y + 0.366);

    fragColor = vec4(ledColor + waveColor + waveColor2, 1.0);
}
