// Shared filmic composite for every seaside pane. Before including, define:
//   EXPOSURE    float scene exposure multiplier
//   BLOOM_TINT  vec3 tint applied to the bloom halo
//   BLOOM_GAIN  float bloom strength
// optionally STREAK_GAIN/STREAK_TINT for an anamorphic streak,
// and optionally HAS_GRADE with `vec3 paneGrade(vec3 col, vec2 uv, float t)`
// applied after tone mapping, in linear space.
#include "default_parameters.h"
#include "common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(set = 1, binding = 0) uniform sampler2D Scene;
layout(set = 1, binding = 1) uniform sampler2D BloomA;
layout(set = 1, binding = 2) uniform sampler2D BloomB;

vec3 acesFilm(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    float t = sceneTime();
    vec2 res = max(ubo.iResolution.xy, vec2(1.0));
    vec2 centered = uv - 0.5;
    float r2 = dot(centered, centered);

    // A whisper of lateral chromatic aberration toward the frame edge.
    vec2 shift = centered * r2 * 0.006;
    vec3 scene;
    scene.r = texture(Scene, uv + shift).r;
    scene.g = texture(Scene, uv).g;
    scene.b = texture(Scene, uv - shift).b;

    vec3 bloom = texture(BloomA, uv).rgb * 0.7 + texture(BloomB, uv).rgb * 1.2;
    vec3 col = scene + bloom * BLOOM_TINT * BLOOM_GAIN;

#ifdef STREAK_GAIN
    // Horizontal anamorphic streak from the wide bloom.
    vec3 streak = vec3(0.0);
    for (int i = -7; i <= 7; ++i)
        streak += texture(BloomB, uv + vec2(float(i) * 0.02, 0.0)).rgb * exp(-abs(float(i)) * 0.4);
    col += streak * STREAK_TINT * STREAK_GAIN;
#endif

    col = acesFilm(col * EXPOSURE);

#ifdef HAS_GRADE
    col = paneGrade(col, uv, t);
#endif

    // Soft lens vignette, gamma, and fine film grain.
    col *= 1.0 - 0.38 * smoothstep(0.12, 0.7, r2);
    col = pow(max(col, 0.0), vec3(1.0 / 2.2));
    col += (hash12(uv * res + fract(t * 0.37) * 91.0) - 0.5) * 0.018;

    fragColor = vec4(col, 1.0);
}
