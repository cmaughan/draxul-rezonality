// SUNSET sky, horizon haze and distant islands, shared by the backdrop, the
// ocean reflections and the boat.
#ifndef SEASIDE_SUNSET_SKY_GLSL
#define SEASIDE_SUNSET_SKY_GLSL

const vec3 SUN_DIR = normalize(vec3(-0.55, 0.045, -1.0));
const vec3 SUN_COL = vec3(1.0, 0.52, 0.22) * 6.0;

// The boat sails toward -X, so the sea flows past it toward +X.
#define WAVE_WIND 0.55
#define WAVE_AMP 0.95
#define BOAT_SPEED 2.4
#define WAVE_FLOW vec2(-BOAT_SPEED, 0.0)

vec3 horizonColor(vec3 rd)
{
    float sunAmount = max(dot(normalize(vec3(rd.x, 0.0, rd.z)), normalize(vec3(SUN_DIR.x, 0.0, SUN_DIR.z))), 0.0);
    return mix(vec3(0.42, 0.15, 0.2), vec3(1.15, 0.42, 0.14), pow(sunAmount, 5.0));
}

vec3 sunsetSkyNoClouds(vec3 rd)
{
    float y = max(rd.y, 0.0);
    float sunAmount = max(dot(rd, SUN_DIR), 0.0);
    vec3 col = horizonColor(rd);
    col = mix(col, vec3(0.3, 0.09, 0.2), smoothstep(0.0, 0.14, y));
    col = mix(col, vec3(0.025, 0.04, 0.14), smoothstep(0.06, 0.42, y));
    col += vec3(1.0, 0.38, 0.1) * pow(sunAmount, 10.0) * 0.6;
    col += vec3(1.0, 0.5, 0.2) * pow(sunAmount, 150.0) * 0.7;
    return col;
}

vec3 sunsetSky(vec3 rd, float t)
{
    vec3 col = sunsetSkyNoClouds(rd);
    float sunAmount = max(dot(rd, SUN_DIR), 0.0);
    // Sun disc, reddened and slightly squashed near the horizon.
    vec3 sd = rd - SUN_DIR;
    float disc = length(vec2(dot(sd.xz, normalize(vec2(-SUN_DIR.z, SUN_DIR.x))), sd.y * 1.12));
    col += vec3(1.0, 0.62, 0.3) * smoothstep(0.0125, 0.0105, disc) * 22.0;

    if (rd.y > 0.0)
    {
        vec2 cp = rd.xz / (rd.y + 0.04);
        float lit = pow(sunAmount, 4.0);
        // Long stratocumulus bands, glowing underneath.
        float band = fbm2(cp * vec2(0.3, 0.75) + vec2(t * 0.004, 0.0), 6);
        float cover = smoothstep(0.55, 0.8, band) * smoothstep(0.015, 0.09, rd.y) * smoothstep(0.75, 0.25, rd.y);
        float thick = smoothstep(0.6, 0.95, band);
        vec3 under = mix(vec3(0.5, 0.14, 0.18), vec3(1.3, 0.55, 0.2), lit) * (0.45 + 1.5 * lit);
        vec3 top = vec3(0.12, 0.06, 0.13);
        vec3 cloud = mix(under, top, thick * 0.7);
        col = mix(col, cloud, cover * 0.9);
        // Wispy high cirrus catching the last pink light.
        float cirrus = fbm2(cp * vec2(0.08, 0.5) + vec2(-t * 0.002, 2.0), 5);
        cirrus = smoothstep(0.55, 0.8, cirrus) * smoothstep(0.1, 0.35, rd.y);
        col += vec3(0.8, 0.25, 0.4) * cirrus * 0.2;
    }
    return col;
}

// Distant islands: hazy silhouettes sitting on the horizon. Returns
// coverage and fills colour.
float islands(vec3 rd, out vec3 col)
{
    float az = atan(rd.x, -rd.z);
    float h = 0.0;
    h = max(h, (0.022 + 0.012 * fbm2(vec2(az * 9.0, 1.0), 4)) * smoothstep(0.55, 0.62, az) * smoothstep(1.15, 0.95, az));
    h = max(h, (0.012 + 0.006 * fbm2(vec2(az * 14.0, 4.0), 3)) * smoothstep(-0.25, -0.18, az) * smoothstep(0.12, 0.02, az));
    h = max(h, (0.035 + 0.02 * fbm2(vec2(az * 6.0, 7.0), 4)) * smoothstep(-1.05, -0.95, az) * smoothstep(-0.6, -0.72, az));
    float cover = step(-0.0005, rd.y) * step(rd.y, h);
    col = mix(vec3(0.28, 0.12, 0.2), horizonColor(rd) * 0.75, 0.45);
    return cover;
}

// Aerial perspective toward the horizon colour.
vec3 seaHaze(vec3 col, vec3 rd, float dist)
{
    float a = 1.0 - exp(-dist * 0.0011);
    return mix(col, horizonColor(rd) * 0.9, a);
}

#endif
