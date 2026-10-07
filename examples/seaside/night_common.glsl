// NIGHT shared lighting: moon, sky, island shoreline and the lighthouse lamp.
#ifndef SEASIDE_NIGHT_COMMON_GLSL
#define SEASIDE_NIGHT_COMMON_GLSL

const vec3 MOON_DIR = normalize(vec3(0.85, 0.3, -0.75));
const vec3 MOON_COL = vec3(0.55, 0.68, 1.0) * 1.1;
const vec3 LAMP_POS = vec3(0.0, 23.25, 0.0);
const vec3 LAMP_COL = vec3(1.0, 0.82, 0.55);
const vec3 BIO_COL = vec3(0.1, 0.75, 1.0);

#define WAVE_WIND -0.4
#define WAVE_AMP 0.72

// Two opposed beams sweeping once every ~9 seconds, dipped toward the sea.
vec3 beamDir(float t, float which)
{
    float a = t * 0.7 + which * PI;
    return normalize(vec3(cos(a), -0.045, sin(a)));
}

// Island shoreline radius; mirrors shoreline_radius() in generate_models.py.
float shorelineRadius(float a)
{
    return 17.0 + 3.0 * sin(3.0 * a + 1.0) + 2.0 * sin(5.0 * a + 2.0) + 1.2 * sin(9.0 * a + 0.5);
}

// Metres from the shoreline: positive out at sea.
float shoreDistance(vec2 xz)
{
    return length(xz) - shorelineRadius(atan(xz.y, xz.x));
}

vec3 nightSkyBase(vec3 rd)
{
    float y = max(rd.y, 0.0);
    float moonAmount = max(dot(rd, MOON_DIR), 0.0);
    vec3 col = mix(vec3(0.02, 0.035, 0.075), vec3(0.004, 0.007, 0.022), smoothstep(0.0, 0.45, y));
    col += vec3(0.25, 0.32, 0.5) * pow(moonAmount, 16.0) * 0.12;
    col += vec3(0.5, 0.6, 0.8) * pow(moonAmount, 300.0) * 0.4;
    return col;
}

vec3 nightStars(vec3 rd, float t)
{
    vec3 col = vec3(0.0);
    for (int layer = 0; layer < 3; ++layer)
    {
        float scale = 110.0 + float(layer) * 130.0;
        vec3 p = rd * scale;
        vec3 cell = floor(p);
        vec3 h = hash33(cell + float(layer) * 17.0);
        if (h.x > 0.16)
            continue;
        vec3 star = cell + 0.25 + 0.5 * h;
        float d = length(p - star);
        float mag = pow(h.y, 7.0) * 2.5 + 0.08;
        float twinkle = 0.7 + 0.3 * sin(t * (1.5 + 4.0 * h.z) + h.x * 50.0);
        vec3 tint = mix(vec3(1.0, 0.8, 0.6), vec3(0.7, 0.85, 1.0), h.z);
        col += tint * mag * twinkle * exp(-d * d * 30.0);
    }
    return col;
}

vec3 nightSky(vec3 rd, float t)
{
    vec3 col = nightSkyBase(rd);
    if (rd.y < 0.0)
        return col;
    // Milky Way: a tilted band of dust and unresolved stars.
    vec3 axis = normalize(vec3(0.3, 0.55, 0.78));
    float band = exp(-pow(dot(rd, axis) / 0.2, 2.0));
    float dust = fbm2(vec2(atan(rd.x, rd.z) * 3.0, rd.y * 6.0), 5);
    col += vec3(0.11, 0.1, 0.14) * band * smoothstep(0.3, 0.8, dust) * smoothstep(0.0, 0.25, rd.y);
    col += nightStars(rd, t) * (0.6 + 1.6 * band) * smoothstep(0.0, 0.12, rd.y);

    // Moon with soft maria, and thin moonlit clouds.
    float md = acos(clamp(dot(rd, MOON_DIR), -1.0, 1.0));
    if (md < 0.06)
    {
        vec3 side = normalize(cross(MOON_DIR, vec3(0.0, 1.0, 0.0)));
        vec3 up = cross(side, MOON_DIR);
        vec2 q = vec2(dot(rd, side), dot(rd, up)) / 0.03;
        float disc = smoothstep(1.0, 0.94, length(q));
        float maria = fbm2(q * 2.2 + 4.0, 4);
        vec3 moon = vec3(1.0, 0.97, 0.9) * (0.75 - 0.35 * smoothstep(0.45, 0.7, maria)) * 4.0;
        col = mix(col, moon, disc);
    }
    vec2 cp = rd.xz / (rd.y + 0.05);
    float cloud = smoothstep(0.55, 0.85, fbm2(cp * vec2(0.25, 0.6) + vec2(t * 0.01, 0.0), 5));
    cloud *= smoothstep(0.02, 0.12, rd.y) * smoothstep(0.6, 0.25, rd.y);
    float lit = pow(max(dot(rd, MOON_DIR), 0.0), 6.0);
    col = mix(col, vec3(0.025, 0.035, 0.06) + vec3(0.3, 0.36, 0.52) * lit, cloud * 0.75);
    return col;
}

// Faint moonlit haze toward the horizon.
vec3 nightHaze(vec3 col, vec3 rd, float dist)
{
    float a = 1.0 - exp(-dist * 0.0016);
    return mix(col, nightSkyBase(vec3(rd.x, 0.0, rd.z)) * 1.1, a);
}

#endif
