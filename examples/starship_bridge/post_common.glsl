// Shared composite for every bridge pane. Before including, define:
//   ACCENT        vec3 HUD tint
//   PANE_LABEL_ID integer seed for the HUD tick pattern
// and optionally HAS_OVERLAY with `vec3 paneOverlay(vec2 p, float aspect, float t)`.
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

float hudLine(float d, float w)
{
    return smoothstep(w, 0.0, abs(d));
}

// Corner brackets, edge tick rulers and a slow scanning bar.
vec3 hudFrame(vec2 p, float aspect, float t)
{
    p.y = 1.0 - p.y; // screen uv origin is top-left; lay the HUD out bottom-up
    vec2 q = vec2(p.x * aspect, p.y);
    vec2 size = vec2(aspect, 1.0);
    float px = 1.5 / ubo.iResolution.y;
    float m = 0.035;
    vec3 col = vec3(0.0);

    // Corner brackets.
    vec2 c = min(q, size - q);
    float arm = 0.07;
    float bracket = 0.0;
    bracket += hudLine(c.x - m, px) * step(c.y, m + arm) * step(m - px, c.y);
    bracket += hudLine(c.y - m, px) * step(c.x, m + arm) * step(m - px, c.x);
    col += ACCENT * min(bracket, 1.0) * 1.2;

    // Tick ruler along the bottom and right edge.
    float tickX = step(0.82, fract(q.x * 40.0)) * step(abs(q.y - (m * 0.5)), 0.006 + 0.008 * step(0.9, fract(q.x * 4.0)));
    float tickY = step(0.82, fract(q.y * 40.0)) * step(abs(q.x - (aspect - m * 0.5)), 0.006 + 0.008 * step(0.9, fract(q.y * 4.0)));
    float ruler = (tickX * step(m + arm, q.x) * step(q.x, aspect - m - arm))
        + (tickY * step(m + arm, q.y) * step(q.y, 1.0 - m - arm));
    col += ACCENT * ruler * 0.45;

    // Tiny status blocks in the top-left, flickering like live readouts.
    vec2 b = (q - vec2(m + 0.02, 1.0 - m - 0.045)) / vec2(0.012, 0.01);
    if (b.x >= 0.0 && b.y >= 0.0 && b.x < 14.0 && b.y < 2.0)
    {
        vec2 cell = floor(b);
        vec2 f = fract(b);
        float on = step(0.35, hash12(cell + floor(t * (2.0 + cell.y)) + float(PANE_LABEL_ID) * 13.0));
        col += ACCENT * on * step(0.15, f.x) * step(0.2, f.y) * step(f.x, 0.85) * step(f.y, 0.8) * 0.8;
    }

    // Scanning bar.
    float bar = exp(-abs(p.y - fract(t * 0.09 + float(PANE_LABEL_ID) * 0.21)) * 220.0);
    col += ACCENT * bar * 0.05;
    return col;
}

void main()
{
    float t = sceneTime();
    vec2 res = max(ubo.iResolution.xy, vec2(1.0));
    float aspect = res.x / res.y;
    vec2 centered = uv - 0.5;

    // Radial chromatic aberration, strongest toward the edges.
    float r2 = dot(centered, centered);
    vec2 shift = centered * r2 * 0.012;
    vec3 scene;
    scene.r = texture(Scene, uv + shift).r;
    scene.g = texture(Scene, uv).g;
    scene.b = texture(Scene, uv - shift).b;

    vec3 bloom = texture(BloomA, uv).rgb * 0.9 + texture(BloomB, uv).rgb * 1.3;
    vec3 col = scene + bloom * 0.85;

    // Anamorphic streak from the wide bloom for hot highlights.
    vec3 streak = vec3(0.0);
    for (int i = -6; i <= 6; ++i)
    {
        float w = exp(-abs(float(i)) * 0.45);
        streak += texture(BloomB, uv + vec2(float(i) * 0.018, 0.0)).rgb * w;
    }
    col += streak * vec3(0.35, 0.55, 1.0) * 0.08;

    col = acesFilm(col * 1.05);

#ifdef HAS_OVERLAY
    col += paneOverlay(uv, aspect, t);
#endif
    col += hudFrame(uv, aspect, t);

    // Scanlines, vignette and fine grain.
    float scan = 0.94 + 0.06 * sin(uv.y * res.y * PI);
    col *= scan;
    col *= 1.0 - 0.55 * smoothstep(0.18, 0.62, r2);
    col = pow(max(col, 0.0), vec3(1.0 / 2.2));
    col += (hash12(uv * res + fract(t) * 91.0) - 0.5) * 0.012;

    fragColor = vec4(col, 1.0);
}
