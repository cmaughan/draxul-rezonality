#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"

// ENGINEERING: the warp core. A volumetric plasma column inside a glass
// containment tube, counter-rotating magnetic constrictor rings, a polished
// deck reflecting it all, and arcs crawling between the rings.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 ray;
layout(location = 0) out vec4 fragColor;

const float CORE_R = 0.32;
const float FLOOR_Y = -2.2;
const int RING_COUNT = 7;

float sdTorus(vec3 p, vec2 r)
{
    vec2 q = vec2(length(p.xz) - r.x, p.y);
    return length(q) - r.y;
}

float sdBox(vec3 p, vec3 b)
{
    vec3 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, max(d.y, d.z)), 0.0);
}

float ringY(int i)
{
    return -1.6 + float(i) * 0.55;
}

// Solid machinery. Returns distance; id in .y (1 rings, 2 floor, 3 struts, 4 caps).
vec2 map(vec3 p, float t)
{
    vec2 res = vec2(p.y - FLOOR_Y, 2.0);

    // Constrictor rings: segmented tori that rotate alternately.
    float rings = 1e9;
    for (int i = 0; i < RING_COUNT; ++i)
    {
        vec3 q = p - vec3(0.0, ringY(i), 0.0);
        float dir = (i % 2 == 0) ? 1.0 : -1.0;
        q.xz = rot2(t * 0.6 * dir + float(i)) * q.xz;
        float a = atan(q.z, q.x);
        float seg = abs(fract(a / TAU * 8.0) - 0.5);
        float torus = sdTorus(q, vec2(0.62, 0.07));
        float block = sdBox(vec3(length(q.xz) - 0.62, q.y, 0.0), vec3(0.11, 0.05, 1.0));
        float segmented = max(min(torus, block), 0.06 - seg * 0.5);
        rings = min(rings, segmented);
    }
    if (rings < res.x)
        res = vec2(rings, 1.0);

    // Four vertical support struts.
    vec3 sp = p;
    sp.xz = abs(sp.xz);
    float strut = sdBox(sp - vec3(0.95, 0.0, 0.95) * 0.75, vec3(0.05, 2.4, 0.05));
    if (strut < res.x)
        res = vec2(strut, 3.0);

    // Top and bottom housings.
    float caps = min(
        sdTorus(p - vec3(0.0, FLOOR_Y + 0.15, 0.0), vec2(0.55, 0.18)),
        sdTorus(p - vec3(0.0, 2.25, 0.0), vec2(0.5, 0.16)));
    if (caps < res.x)
        res = vec2(caps, 4.0);
    return res;
}

vec3 calcNormal(vec3 p, float t)
{
    vec2 e = vec2(0.0015, 0.0);
    return normalize(vec3(
        map(p + e.xyy, t).x - map(p - e.xyy, t).x,
        map(p + e.yxy, t).x - map(p - e.yxy, t).x,
        map(p + e.yyx, t).x - map(p - e.yyx, t).x));
}

// Plasma emission density at p.
vec3 plasma(vec3 p, float t)
{
    float r = length(p.xz);
    if (r > CORE_R || abs(p.y) > 2.1)
        return vec3(0.0);
    vec3 q = p;
    q.xz = rot2(p.y * 2.0 - t * 1.5) * q.xz;
    float turb = fbm3(vec3(q.xz * 5.0, p.y * 2.0 - t * 2.5), 4);
    float pulse = pow(0.5 + 0.5 * sin(p.y * 4.0 - t * 5.0), 6.0);
    float falloff = smoothstep(CORE_R, 0.0, r);
    float filament = smoothstep(0.55, 0.75, turb);
    vec3 c = mix(vec3(0.05, 0.35, 1.0), vec3(0.55, 0.85, 1.0), pulse);
    c = mix(c, vec3(1.0), filament * falloff * 0.6);
    return c * falloff * (0.6 + 2.8 * pulse + 1.6 * filament);
}

vec3 shade(vec3 p, vec3 n, vec3 rd, float id, float t)
{
    // Light from the nearest point of the plasma column.
    vec3 lp = vec3(0.0, clamp(p.y, -2.0, 2.0), 0.0);
    vec3 l = lp - p;
    float dist = length(l);
    l /= dist;
    float pulse = 0.75 + 0.25 * sin(p.y * 4.0 - t * 5.0);
    vec3 lightCol = vec3(0.3, 0.6, 1.0) * pulse * 3.0 / (1.0 + dist * dist * 1.5);
    float ndl = max(dot(n, l), 0.0);
    vec3 h = normalize(l - rd);
    float spec = pow(max(dot(n, h), 0.0), 48.0);

    vec3 albedo = vec3(0.18, 0.19, 0.22);
    vec3 emissive = vec3(0.0);
    if (id < 1.5)
    {
        albedo = vec3(0.35, 0.3, 0.26);
        // Glowing inner face of each constrictor ring.
        float inner = smoothstep(0.62, 0.52, length(p.xz));
        emissive = vec3(1.0, 0.45, 0.12) * inner * (1.5 + 1.0 * sin(t * 3.0 + p.y * 5.0));
    }
    else if (id < 2.5)
    {
        vec2 g = p.xz * 2.0;
        vec2 f = abs(fract(g) - 0.5);
        float seam = smoothstep(0.48, 0.5, max(f.x, f.y));
        albedo = vec3(0.05, 0.055, 0.07) * (1.0 - seam * 0.5);
        float r = length(p.xz);
        float rings = smoothstep(0.02, 0.0, abs(fract(r * 1.5 - t * 0.3) - 0.5) - 0.47);
        emissive = vec3(0.1, 0.4, 1.0) * rings * exp(-r * 0.5) * 0.6;
        // Hazard stripes around the core pit.
        float hz = step(0.5, fract((p.x + p.z) * 4.0)) * step(abs(r - 1.25), 0.08);
        albedo = mix(albedo, vec3(0.5, 0.35, 0.02), hz);
    }
    else if (id < 3.5)
    {
        float lightStrip = step(0.9, fract(p.y * 1.5 + t * 0.5));
        emissive = vec3(1.0, 0.3, 0.1) * lightStrip * 0.8 * step(0.04, abs(p.x) + abs(p.z));
    }
    return albedo * (lightCol * ndl + vec3(0.02, 0.025, 0.04)) + lightCol * spec * 0.8 + emissive;
}

vec3 render(vec3 ro, vec3 rd, float t, bool reflection)
{
    // The bay is a cylinder of radius 6; it bounds every march.
    float wa = max(dot(rd.xz, rd.xz), 1e-6);
    float wb = dot(ro.xz, rd.xz);
    float wc = dot(ro.xz, ro.xz) - 36.0;
    float wh = max(wb * wb - wa * wc, 0.0);
    float wallT = max((-wb + sqrt(wh)) / wa, 0.0);
    float maxT = min(wallT, 30.0);

    vec3 glow = vec3(0.0);
    float travelled = 0.0;
    vec2 hit = vec2(-1.0);
    int steps = reflection ? 64 : 110;
    for (int i = 0; i < steps; ++i)
    {
        vec3 p = ro + rd * travelled;
        vec2 d = map(p, t);
        // Volumetric plasma and halo sampled along the march.
        float r = length(p.xz);
        float stepLen = clamp(min(d.x, max(r - CORE_R, 0.02)), 0.02, 0.35);
        glow += plasma(p, t) * stepLen * 1.4;
        glow += vec3(0.1, 0.35, 1.0) * exp(-max(r - CORE_R, 0.0) * 7.0) * stepLen * 0.18 * step(abs(p.y), 2.1);
        if (d.x < 0.001 * (1.0 + travelled))
        {
            hit = vec2(travelled, d.y);
            break;
        }
        travelled += min(d.x, stepLen);
        if (travelled > maxT)
            break;
    }

    vec3 col = vec3(0.004, 0.006, 0.012);
    if (hit.x > 0.0)
    {
        vec3 p = ro + rd * hit.x;
        vec3 n = calcNormal(p, t);
        col = shade(p, n, rd, hit.y, t);
        col *= exp(-hit.x * 0.05);
    }
    else
    {
        // Engineering bay: a cylindrical wall of lit panels around the core.
        vec3 pw = ro + rd * wallT;
        float ang = atan(pw.z, pw.x) / TAU * 32.0;
        float px = fract(ang);
        float frame = step(0.06, px) * step(px, 0.94);
        float strip = smoothstep(0.03, 0.0, abs(px - 0.5) - 0.04) * smoothstep(3.0, 0.5, abs(pw.y));
        float ribs = step(0.9, fract(pw.y * 1.2));
        float lit = 0.4 + 0.6 * hash11(floor(ang));
        col += vec3(0.02, 0.03, 0.05) * frame * (1.0 - ribs) * smoothstep(5.0, 0.0, abs(pw.y));
        col += vec3(0.15, 0.35, 0.8) * strip * lit * 0.35 * exp(-wallT * 0.04);
    }

    // Glass containment tube highlight.
    vec3 oc = ro;
    float a = dot(rd.xz, rd.xz);
    float b = dot(oc.xz, rd.xz);
    float c = dot(oc.xz, oc.xz) - 0.36 * 0.36;
    float h = b * b - a * c;
    if (h > 0.0)
    {
        float tg = (-b - sqrt(h)) / a;
        vec3 pg = ro + rd * tg;
        if (tg > 0.0 && abs(pg.y) < 2.1 && (hit.x < 0.0 || tg < hit.x))
        {
            vec3 ng = normalize(vec3(pg.x, 0.0, pg.z));
            float fres = pow(1.0 - abs(dot(ng, rd)), 3.0);
            col += vec3(0.4, 0.7, 1.0) * fres * 0.6;
        }
    }

    // Electrical arcs jumping between neighbouring rings.
    if (!reflection)
    {
        for (int i = 0; i < RING_COUNT - 1; ++i)
        {
            float seed = floor(t * 6.0) + float(i) * 7.0;
            if (hash11(seed) < 0.55)
                continue;
            float ang = hash11(seed + 1.3) * TAU;
            vec3 a0 = vec3(cos(ang) * 0.55, ringY(i), sin(ang) * 0.55);
            vec3 a1 = vec3(cos(ang + 0.4) * 0.55, ringY(i + 1), sin(ang + 0.4) * 0.55);
            // Closest approach between the view ray and the jittered arc segment.
            vec3 ba = a1 - a0;
            vec3 oa = ro - a0;
            float rb = dot(rd, ba);
            float bb = dot(ba, ba);
            float denom = bb - rb * rb;
            float sArc = clamp((dot(oa, ba) - rb * dot(oa, rd)) / max(denom, 1e-4), 0.0, 1.0);
            vec3 pa = a0 + ba * sArc;
            pa += (vec3(noise2(vec2(sArc * 12.0, seed)), 0.0, noise2(vec2(seed, sArc * 12.0))) - 0.5) * 0.12;
            float tr = dot(pa - ro, rd);
            float d = length(ro + rd * tr - pa);
            col += vec3(0.6, 0.8, 1.0) * exp(-d * 260.0) * 3.0 + vec3(0.2, 0.4, 1.0) * exp(-d * 40.0) * 0.25;
        }
    }
    return col + glow;
}

void main()
{
    float t = sceneTime();
    vec3 rd = normalize(ray);
    vec3 ro = cameraOrigin();
    vec3 col = render(ro, rd, t, false);

    // Polished deck: add a blurred reflection of the core.
    if (rd.y < 0.0)
    {
        float tf = (FLOOR_Y - ro.y) / rd.y;
        vec3 p = ro + rd * tf;
        vec2 hit = map(p + vec3(0.0, 0.01, 0.0), t);
        if (dot(p.xz, p.xz) < 36.0 && hit.y > 1.5 && hit.y < 2.5 && hit.x < 0.05)
        {
            vec3 rr = reflect(rd, vec3(0.0, 1.0, 0.0));
            float fres = 0.15 + 0.5 * pow(1.0 - abs(rd.y), 4.0);
            col += render(p + vec3(0.0, 0.02, 0.0), rr, t, true) * fres * 0.6;
        }
    }

    fragColor = vec4(col, 1.0);
}
