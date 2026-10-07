// Shared helpers for the seaside panes.
#ifndef SEASIDE_COMMON_GLSL
#define SEASIDE_COMMON_GLSL

#define PI 3.14159265359
#define TAU 6.28318530718

// Scene clock: offset so a paused/first frame is already mid-animation.
float sceneTime()
{
    return ubo.iTime + 20.0;
}

mat2 rot2(float a)
{
    float c = cos(a), s = sin(a);
    return mat2(c, -s, s, c);
}

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    return fract(p * (p + p));
}

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 hash22(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

vec3 hash33(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

float noise2(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), f.x),
        mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), f.x), f.y);
}

float fbm2(vec2 p, int octaves)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < octaves; ++i)
    {
        sum += amp * noise2(p);
        p = rot2(0.6) * p * 2.02 + 3.7;
        amp *= 0.5;
    }
    return sum;
}

float noise3(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    vec2 o = i.xy + i.z * vec2(37.17, 17.71);
    vec2 o1 = o + vec2(37.17, 17.71);
    float a = mix(mix(hash12(o), hash12(o + vec2(1, 0)), f.x),
        mix(hash12(o + vec2(0, 1)), hash12(o + vec2(1, 1)), f.x), f.y);
    float b = mix(mix(hash12(o1), hash12(o1 + vec2(1, 0)), f.x),
        mix(hash12(o1 + vec2(0, 1)), hash12(o1 + vec2(1, 1)), f.x), f.y);
    return mix(a, b, f.z);
}

float fbm3(vec3 p, int octaves)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < octaves; ++i)
    {
        sum += amp * noise3(p);
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        amp *= 0.5;
    }
    return sum;
}

// Cellular distance (F1) for caustics, cracks and pebbles.
float voronoi2(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    float d = 8.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 g = vec2(x, y);
            vec2 o = hash22(i + g);
            vec2 r = g + o - f;
            d = min(d, dot(r, r));
        }
    }
    return sqrt(d);
}

float smin(float a, float b, float k)
{
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// World-space camera position for screen_rect passes that declare a camera.
vec3 cameraOrigin()
{
    return ubo.viewInverse[3].xyz;
}

// Clip-space depth of a world position, for raymarched passes that share a
// depth buffer with rasterised models.
float worldDepth(vec3 p)
{
    vec4 clip = ubo.projection * ubo.view * vec4(p, 1.0);
    return clip.z / clip.w;
}

// Soaring gull silhouette in a camera-facing plane. q is in gull-local units
// (wingspan ~2), flap in [-1, 1]. Returns coverage.
float gullShape(vec2 q, float flap)
{
    q.x = abs(q.x);
    // Each wing is a gentle M: rises from the body, then droops to the tip.
    float lift = 0.35 * flap;
    float y = lift * q.x * (1.6 - q.x) - 0.25 * q.x * q.x * q.x * 0.6;
    float thickness = mix(0.12, 0.03, smoothstep(0.0, 1.0, q.x));
    float wing = smoothstep(thickness, thickness * 0.4, abs(q.y - y)) * step(q.x, 1.0);
    float body = smoothstep(0.12, 0.06, length(q * vec2(1.0, 1.6)));
    return max(wing, body);
}

#endif
