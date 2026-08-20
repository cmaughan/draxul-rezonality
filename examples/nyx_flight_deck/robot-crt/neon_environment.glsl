const float NEON_PI = 3.14159265359;

float neonHash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec2 neonHash22(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float neonNoise3(vec3 p)
{
    vec3 cell = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    return mix(
        mix(mix(neonHash13(cell), neonHash13(cell + vec3(1.0, 0.0, 0.0)), f.x),
            mix(neonHash13(cell + vec3(0.0, 1.0, 0.0)), neonHash13(cell + vec3(1.0, 1.0, 0.0)), f.x), f.y),
        mix(mix(neonHash13(cell + vec3(0.0, 0.0, 1.0)), neonHash13(cell + vec3(1.0, 0.0, 1.0)), f.x),
            mix(neonHash13(cell + vec3(0.0, 1.0, 1.0)), neonHash13(cell + vec3(1.0)), f.x), f.y), f.z);
}

float neonFbm(vec3 p)
{
    float value = 0.0;
    float amplitude = 0.52;
    mat3 basis = mat3(
         0.00,  0.80,  0.60,
        -0.80,  0.36, -0.48,
        -0.60, -0.48,  0.64);

    for (int octave = 0; octave < 6; ++octave) {
        value += amplitude * neonNoise3(p);
        p = basis * p * 2.03 + vec3(7.1, 3.7, 11.9);
        amplitude *= 0.49;
    }
    return value;
}

float neonStarLayer(vec2 uv, float scale, float threshold, float time)
{
    vec2 grid = uv * scale;
    vec2 cell = floor(grid);
    vec2 local = fract(grid) - 0.5;
    vec2 random = neonHash22(cell);
    vec2 center = (random - 0.5) * 0.72;
    float seed = neonHash13(vec3(cell, scale));
    float radius = mix(0.018, 0.07, seed * seed);
    float distanceToStar = length(local - center);
    float star = smoothstep(radius, radius * 0.12, distanceToStar);
    float twinkle = 0.72 + 0.28 * sin(time * (1.5 + seed * 4.0) + seed * 31.0);
    return star * twinkle * smoothstep(threshold, 1.0, seed);
}

vec3 neonRibbon(vec2 uv, float time, float phase, vec3 color)
{
    float detail = neonNoise3(vec3(uv.x * 12.0, phase, time * 0.07));
    float center = 0.53
        + 0.055 * sin(uv.x * 11.0 + time * 0.32 + phase)
        + 0.025 * sin(uv.x * 27.0 - time * 0.19 + phase * 2.3)
        + (detail - 0.5) * 0.07;
    float distanceToRibbon = abs(uv.y - center);
    float bloom = exp(-distanceToRibbon * 42.0);
    float body = exp(-distanceToRibbon * 135.0);
    float core = exp(-distanceToRibbon * 520.0);
    return color * (bloom * 0.18 + body * 0.75 + core * 2.8);
}

vec3 neonEnvironment(vec3 direction, float time)
{
    vec3 ray = normalize(direction);
    vec2 uv = vec2(atan(ray.z, ray.x) / (2.0 * NEON_PI) + 0.5,
                   asin(clamp(ray.y, -1.0, 1.0)) / NEON_PI + 0.5);

    float vertical = smoothstep(-0.7, 0.9, ray.y);
    vec3 color = mix(vec3(0.001, 0.002, 0.014), vec3(0.018, 0.008, 0.075), vertical);

    vec3 cloudPoint = ray * 4.2 + vec3(time * 0.018, -time * 0.011, time * 0.014);
    vec3 warp = vec3(
        neonFbm(cloudPoint + vec3(1.7, 8.2, 2.4)),
        neonFbm(cloudPoint + vec3(9.3, 3.1, 7.6)),
        neonFbm(cloudPoint + vec3(4.8, 11.4, 5.2)));
    float nebula = neonFbm(cloudPoint * 1.25 + (warp - 0.5) * 3.1);
    float fineCloud = neonFbm(cloudPoint * 3.4 + warp * 1.7);
    float cloudMask = smoothstep(0.38, 0.82, nebula);
    float filaments = pow(clamp(1.0 - abs(fineCloud * 2.0 - 1.0), 0.0, 1.0), 7.0);

    vec3 violet = vec3(0.31, 0.025, 0.78);
    vec3 cyan = vec3(0.01, 0.74, 1.25);
    vec3 magenta = vec3(1.15, 0.015, 0.52);
    vec3 nebulaColor = mix(violet, cyan, smoothstep(0.42, 0.76, warp.y));
    nebulaColor = mix(nebulaColor, magenta, smoothstep(0.58, 0.88, warp.x));
    color += nebulaColor * cloudMask * (0.22 + nebula * 0.82);
    color += mix(cyan, magenta, warp.z) * filaments * cloudMask * 0.48;

    float horizon = exp(-abs(ray.y + 0.10) * 18.0);
    color += mix(vec3(0.02, 0.12, 0.42), vec3(0.72, 0.015, 0.46), uv.x) * horizon * 0.33;

    color += neonRibbon(uv, time, 0.0, vec3(0.0, 0.95, 1.35));
    color += neonRibbon(uv + vec2(0.0, 0.072), time, 2.1, vec3(1.25, 0.01, 0.62));
    color += neonRibbon(uv + vec2(0.0, -0.055), time, 4.4, vec3(0.24, 0.12, 1.35)) * 0.7;

    float starsNear = neonStarLayer(uv, 330.0, 0.975, time);
    float starsFar = neonStarLayer(uv + vec2(0.173, 0.319), 610.0, 0.987, time * 0.73);
    float starStrength = starsNear + starsFar * 0.55;
    vec3 starColor = mix(vec3(0.35, 0.72, 1.4), vec3(1.3, 0.34, 0.92),
                         neonNoise3(ray * 39.0));
    color += starColor * starStrength * (1.1 - cloudMask * 0.42);

    float energyGrain = pow(neonNoise3(ray * 95.0 + warp * 8.0), 10.0);
    color += mix(cyan, magenta, neonNoise3(ray * 17.0)) * energyGrain * 0.38;
    return color;
}
