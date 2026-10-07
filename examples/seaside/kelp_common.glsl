// MIDDAY underwater lighting shared by the kelp-forest passes.
#ifndef SEASIDE_KELP_COMMON_GLSL
#define SEASIDE_KELP_COMMON_GLSL

const float SURFACE_Y = 17.0;
const vec3 SUN_DIR = normalize(vec3(-0.35, 1.0, -0.55));
// Sunlight after refraction at the surface, travelling downward.
const vec3 LIGHT_DIR = normalize(vec3(-SUN_DIR.x / 1.33, -1.0, -SUN_DIR.z / 1.33));
const vec3 SUN_UNDER = vec3(1.0, 0.95, 0.8) * 1.7;

// Colour of open water seen along a direction.
vec3 waterColor(vec3 rd)
{
    vec3 deep = vec3(0.0, 0.008, 0.026);
    vec3 up = vec3(0.012, 0.13, 0.19);
    vec3 col = mix(deep, up, smoothstep(-0.5, 0.9, rd.y));
    col += vec3(0.15, 0.36, 0.3) * pow(max(dot(rd, -LIGHT_DIR), 0.0), 5.0) * 0.6;
    return col;
}

// Light lost on the way down from the surface and across to the eye; red goes first.
vec3 underwater(vec3 col, vec3 p, vec3 rd, float dist)
{
    float path = dist + max(SURFACE_Y - p.y, 0.0) * 0.6;
    col *= exp(-path * vec3(0.065, 0.028, 0.024));
    float fog = 1.0 - exp(-dist * 0.034);
    return mix(col, waterColor(rd), fog);
}

// Tileable water caustic (after Dave Hoskins / joltz0r); period TAU.
float caustic(vec2 p, float t)
{
    vec2 q = mod(p, TAU) - 250.0;
    vec2 i = q;
    float c = 1.0;
    float inten = 0.005;
    for (int n = 0; n < 5; ++n)
    {
        float tt = t * (1.0 - (3.5 / float(n + 1)));
        i = q + vec2(cos(tt - i.x) + sin(tt + i.y), sin(tt - i.y) + cos(tt + i.x));
        c += 1.0 / length(vec2(q.x / (sin(i.x + tt) / inten), q.y / (cos(i.y + tt) / inten)));
    }
    c /= 5.0;
    c = 1.17 - pow(c, 1.4);
    return pow(abs(c), 8.0);
}

// Caustic light arriving at a point, projected up to the surface along the light.
float causticAt(vec3 p, float t)
{
    vec3 s = p - LIGHT_DIR * ((SURFACE_Y - p.y) / -LIGHT_DIR.y);
    float depthFade = exp(-max(SURFACE_Y - p.y, 0.0) * 0.03);
    return caustic(s.xz * 0.32, t * 0.55) * depthFade;
}

float seabedHeight(vec2 xz)
{
    float dunes = 0.45 * sin(xz.x * 0.09 + 0.6 * sin(xz.y * 0.05)) + 0.3 * sin(xz.y * 0.13 + xz.x * 0.04);
    float mounds = smoothstep(0.55, 0.85, fbm2(xz * 0.045 + 3.0, 4)) * 3.2;
    return 0.35 + dunes + mounds + 0.25 * fbm2(xz * 0.3, 3);
}

// Path of the sardine school: a tilted loop that threads the kelp.
vec3 schoolPath(float s)
{
    return vec3(8.5 * cos(s) + 0.5, 6.0 + 1.4 * sin(2.0 * s) + 0.8 * sin(s), -7.0 + 6.0 * sin(s));
}

#endif
