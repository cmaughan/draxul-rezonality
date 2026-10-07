#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"

// NAVIGATION: a banded ice giant with scattering atmosphere, shadowed rings,
// a cratered moon and the plotted approach trajectory.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 ray;
layout(location = 0) out vec4 fragColor;

const vec3 SUN_DIR = normalize(vec3(-0.85, 0.25, 0.45));
const float PLANET_R = 1.6;
const float ATMO_R = 1.78;
const vec3 RING_N = normalize(vec3(0.18, 1.0, -0.22));
const float RING_IN = 2.15;
const float RING_OUT = 3.7;

vec2 sphereHit(vec3 ro, vec3 rd, vec3 c, float r)
{
    vec3 oc = ro - c;
    float b = dot(oc, rd);
    float h = b * b - dot(oc, oc) + r * r;
    if (h < 0.0)
        return vec2(-1.0);
    h = sqrt(h);
    return vec2(-b - h, -b + h);
}

float planeHit(vec3 ro, vec3 rd, vec3 n)
{
    float d = dot(rd, n);
    if (abs(d) < 1e-5)
        return -1.0;
    return -dot(ro, n) / d;
}

float ringDensity(float r)
{
    if (r < RING_IN || r > RING_OUT)
        return 0.0;
    float x = (r - RING_IN) / (RING_OUT - RING_IN);
    float bands = 0.55 + 0.45 * sin(r * 41.0) * sin(r * 13.0 + 1.3);
    bands *= 0.6 + 0.4 * noise2(vec2(r * 60.0, 0.5));
    float gap = smoothstep(0.012, 0.03, abs(x - 0.62)) * smoothstep(0.005, 0.015, abs(x - 0.28));
    float edge = smoothstep(0.0, 0.04, x) * smoothstep(1.0, 0.9, x);
    return clamp(bands * gap * edge, 0.0, 1.0);
}

vec3 moonPos(float t)
{
    float a = t * 0.11 + 2.2;
    return vec3(cos(a) * 5.4, 0.9 + sin(a) * 0.6, sin(a) * 5.4);
}

vec3 planetSurface(vec3 n, float t)
{
    // Rotate the atmosphere bands with the planet's spin.
    vec3 p = n;
    p.xz = rot2(t * 0.04) * p.xz;
    float lat = p.y;
    float warp = fbm3(p * 3.0 + vec3(0.0, 0.0, t * 0.02), 4);
    float bands = sin(lat * 18.0 + warp * 3.5) * 0.5 + 0.5;
    float fine = fbm3(vec3(p.xz * 2.0, lat * 30.0) + warp, 4);
    vec3 deep = vec3(0.04, 0.16, 0.32);
    vec3 mid = vec3(0.16, 0.55, 0.68);
    vec3 pale = vec3(0.72, 0.88, 0.92);
    vec3 col = mix(deep, mid, bands);
    col = mix(col, pale, smoothstep(0.55, 0.85, fine) * 0.6);

    // Great storm.
    vec3 stormCenter = normalize(vec3(0.6, -0.25, 0.75));
    float sd = acos(clamp(dot(p, stormCenter), -1.0, 1.0));
    float swirl = sin(sd * 40.0 - atan(p.y - stormCenter.y, p.x - stormCenter.x) * 2.0 + t * 0.3);
    float storm = smoothstep(0.22, 0.0, sd);
    col = mix(col, mix(vec3(0.9, 0.55, 0.35), vec3(1.0, 0.85, 0.7), swirl * 0.5 + 0.5), storm * 0.85);
    return col;
}

void main()
{
    float t = sceneTime();
    vec3 rd = normalize(ray);
    vec3 ro = cameraOrigin();

    vec3 col = vec3(0.002, 0.003, 0.008);
    col += nebula(rd, vec3(0.30, 0.12, 0.05), vec3(0.06, 0.12, 0.35), t) * 0.6;
    col += starField(rd, 0.2);
    float sunDot = max(dot(rd, SUN_DIR), 0.0);
    col += vec3(1.0, 0.85, 0.65) * (pow(sunDot, 3000.0) * 60.0 + pow(sunDot, 120.0) * 1.2 + pow(sunDot, 8.0) * 0.08);

    float tNear = 1e9;
    vec2 planet = sphereHit(ro, rd, vec3(0.0), PLANET_R);
    vec3 moonC = moonPos(t);
    vec2 moon = sphereHit(ro, rd, moonC, 0.32);

    // Rings: transparent layer composited front-to-back with the planet.
    float tr = planeHit(ro, rd, RING_N);
    vec4 ring = vec4(0.0);
    if (tr > 0.0)
    {
        vec3 p = ro + rd * tr;
        float r = length(p);
        float dens = ringDensity(r);
        if (dens > 0.0)
        {
            vec2 sh = sphereHit(p, SUN_DIR, vec3(0.0), PLANET_R);
            float shadow = sh.x > 0.0 ? 0.08 : 1.0;
            float forward = pow(max(dot(rd, SUN_DIR), 0.0), 6.0);
            vec3 rc = mix(vec3(0.85, 0.72, 0.58), vec3(0.55, 0.62, 0.72), noise2(vec2(r * 9.0, 1.0)));
            rc *= (0.35 + 1.4 * forward) * shadow * 1.6;
            ring = vec4(rc, dens * 0.85);
        }
    }

    vec3 body = vec3(0.0);
    float bodyT = 1e9;
    bool hitBody = false;
    if (planet.x > 0.0)
    {
        bodyT = planet.x;
        hitBody = true;
        vec3 p = ro + rd * planet.x;
        vec3 n = normalize(p);
        float ndl = dot(n, SUN_DIR);
        float lit = smoothstep(-0.12, 0.35, ndl);
        // Ring shadow projected onto the planet.
        float trs = planeHit(p, SUN_DIR, RING_N);
        float ringShadow = trs > 0.0 ? 1.0 - 0.75 * ringDensity(length(p + SUN_DIR * trs)) : 1.0;
        vec3 surf = planetSurface(n, t);
        body = surf * lit * ringShadow * 0.95;
        // Night side aurora and city-like lightning flashes.
        float night = smoothstep(0.1, -0.3, ndl);
        float aurora = pow(abs(n.y), 6.0) * (0.5 + 0.5 * sin(atan(n.z, n.x) * 9.0 + t * 0.8));
        body += vec3(0.1, 1.0, 0.55) * aurora * night * 0.6;
        float flash = step(0.985, hash12(floor(n.xz * 40.0) + floor(t * 3.0))) * night;
        body += vec3(0.7, 0.8, 1.0) * flash * 1.5;
    }
    if (moon.x > 0.0 && moon.x < bodyT)
    {
        bodyT = moon.x;
        hitBody = true;
        vec3 p = ro + rd * moon.x;
        vec3 n = normalize(p - moonC);
        float craters = fbm3(n * 6.0, 5);
        float pits = smoothstep(0.55, 0.6, fbm3(n * 11.0 + 4.0, 3));
        vec3 albedo = vec3(0.55, 0.52, 0.5) * (0.6 + 0.6 * craters) * (1.0 - 0.35 * pits);
        float ps = sphereHit(p, SUN_DIR, vec3(0.0), PLANET_R).x > 0.0 ? 0.05 : 1.0;
        body = albedo * max(dot(n, SUN_DIR), 0.0) * 1.8 * ps;
    }

    // Atmospheric limb glow, integrated along the ray past the planet.
    vec2 atmo = sphereHit(ro, rd, vec3(0.0), ATMO_R);
    vec3 atmoCol = vec3(0.0);
    if (atmo.x > -1e8 && atmo.y > 0.0)
    {
        float t0 = max(atmo.x, 0.0);
        float t1 = planet.x > 0.0 ? planet.x : atmo.y;
        vec3 mid = ro + rd * (0.5 * (t0 + t1));
        float h = (length(mid) - PLANET_R) / (ATMO_R - PLANET_R);
        float density = exp(-max(h, 0.0) * 3.0) * (t1 - t0);
        float sunSide = smoothstep(-0.35, 0.6, dot(normalize(mid), SUN_DIR));
        float mie = pow(max(dot(rd, SUN_DIR), 0.0), 8.0);
        atmoCol = (vec3(0.25, 0.6, 1.0) * sunSide + vec3(1.0, 0.7, 0.4) * mie) * density * 0.9;
    }

    if (hitBody)
        col = body;
    col += atmoCol;
    if (tr > 0.0 && tr < bodyT)
        col = mix(col, ring.rgb, ring.a);

    // Plotted trajectory: a dashed orbit with a ship marker riding it.
    vec3 orbitN = normalize(vec3(-0.25, 1.0, 0.1));
    float to = planeHit(ro, rd, orbitN);
    if (to > 0.0 && to < bodyT)
    {
        vec3 p = ro + rd * to;
        float r = length(p);
        float ang = atan(p.z, p.x);
        float w = 0.005 * to;
        float line = smoothstep(w, 0.0, abs(r - 4.6));
        float dash = step(0.6, fract(ang * 18.0 + t * 0.6));
        col += vec3(1.0, 0.6, 0.15) * line * (dash * 0.9 + 0.08);
        float shipAng = t * 0.25;
        vec3 shipP = vec3(cos(shipAng), 0.0, sin(shipAng)) * 4.6;
        shipP -= orbitN * dot(shipP, orbitN);
        shipP = normalize(shipP) * 4.6;
        float sd = length(p - shipP);
        col += vec3(1.0, 0.75, 0.3) * (exp(-sd * 30.0) * 6.0 + smoothstep(w * 1.5, 0.0, abs(sd - 0.18)) * 1.0);
    }

    fragColor = vec4(col, 1.0);
}
