// Shared helpers for the starship bridge panes.
#ifndef STARSHIP_COMMON_GLSL
#define STARSHIP_COMMON_GLSL

#define PI 3.14159265359
#define TAU 6.28318530718

// Scene clock: offset so a paused/first frame is already mid-animation.
float sceneTime()
{
    return ubo.iTime + 12.0;
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

vec3 hash33(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

float noise3(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash12(i.xy + i.z * 37.17);
    float n100 = hash12(i.xy + vec2(1, 0) + i.z * 37.17);
    float n010 = hash12(i.xy + vec2(0, 1) + i.z * 37.17);
    float n110 = hash12(i.xy + vec2(1, 1) + i.z * 37.17);
    float n001 = hash12(i.xy + (i.z + 1.0) * 37.17);
    float n101 = hash12(i.xy + vec2(1, 0) + (i.z + 1.0) * 37.17);
    float n011 = hash12(i.xy + vec2(0, 1) + (i.z + 1.0) * 37.17);
    float n111 = hash12(i.xy + vec2(1, 1) + (i.z + 1.0) * 37.17);
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
        mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
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

// Point stars from a direction, with colour temperature variation.
vec3 starField(vec3 dir, float density)
{
    vec3 col = vec3(0.0);
    for (int layer = 0; layer < 3; ++layer)
    {
        float scale = 90.0 + float(layer) * 110.0;
        vec3 p = dir * scale;
        vec3 cell = floor(p);
        vec3 h = hash33(cell + float(layer) * 17.0);
        if (h.x > density)
            continue;
        vec3 star = cell + 0.25 + 0.5 * h;
        float d = length(p - star);
        float mag = pow(h.y, 6.0) * 3.0 + 0.15;
        float twinkle = 0.75 + 0.25 * sin(sceneTime() * (1.0 + 3.0 * h.z) + h.x * 40.0);
        vec3 tint = mix(vec3(1.0, 0.75, 0.55), vec3(0.6, 0.8, 1.0), h.z);
        col += tint * mag * twinkle * exp(-d * d * 28.0);
    }
    return col;
}

// Soft coloured nebula; returns HDR radiance.
vec3 nebula(vec3 dir, vec3 colA, vec3 colB, float t)
{
    vec3 p = dir * 2.2 + vec3(0.0, 0.0, t * 0.01);
    float warp = fbm3(p * 1.3, 4);
    float n = fbm3(p * 2.0 + warp * 1.6, 5);
    float dust = smoothstep(0.45, 0.85, fbm3(p * 3.1 - warp, 4));
    vec3 col = mix(colA, colB, smoothstep(0.35, 0.75, warp));
    float density = smoothstep(0.38, 0.9, n);
    return col * density * density * 1.4 * (1.0 - 0.7 * dust);
}

// World-space camera position for screen_rect passes that declare a camera.
vec3 cameraOrigin()
{
    return ubo.viewInverse[3].xyz;
}

#endif
