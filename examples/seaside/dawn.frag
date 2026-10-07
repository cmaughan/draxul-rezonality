#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"

// DAWN - limestone sea stacks and an arch in pastel morning mist, a slow swell,
// a glittering sun path and gulls riding the updraft off the headland.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 ray;
layout(location = 0) out vec4 fragColor;

const vec3 SUN_DIR = normalize(vec3(0.24, 0.028, -1.0));
const vec3 SUN_COL = vec3(1.0, 0.62, 0.38) * 7.0;

// ---------------------------------------------------------------- sky

vec3 skyColor(vec3 rd, float t)
{
    float y = max(rd.y, 0.0);
    float sunAmount = max(dot(rd, SUN_DIR), 0.0);
    // Peach horizon, rose band, lavender zenith.
    vec3 horizon = vec3(1.0, 0.62, 0.45);
    vec3 band = vec3(0.86, 0.5, 0.62);
    vec3 zenith = vec3(0.28, 0.32, 0.58);
    vec3 col = mix(horizon, band, smoothstep(0.0, 0.12, y));
    col = mix(col, zenith, smoothstep(0.08, 0.6, y));
    col *= 0.62;
    // Wide warm glow and the sun disc.
    col += vec3(1.0, 0.5, 0.28) * pow(sunAmount, 6.0) * 0.45;
    col += vec3(1.0, 0.6, 0.36) * pow(sunAmount, 90.0) * 0.22;
    col += vec3(1.0, 0.72, 0.48) * pow(sunAmount, 1500.0) * 0.6;
    col += vec3(1.0, 0.88, 0.7) * smoothstep(0.99990, 0.99994, sunAmount) * 40.0;

    // Two decks of cloud lit from below: thin streaks and puffy cumulus.
    if (rd.y > 0.0)
    {
        vec2 cp = rd.xz / (rd.y + 0.06);
        float streak = fbm2(cp * vec2(0.35, 1.6) + vec2(t * 0.01, 0.0), 5);
        streak = smoothstep(0.52, 0.85, streak) * smoothstep(0.0, 0.08, rd.y);
        float puff = fbm2(cp * 0.55 + vec2(t * 0.006, 3.0), 5);
        puff = smoothstep(0.55, 0.8, puff) * smoothstep(0.02, 0.2, rd.y) * smoothstep(0.7, 0.3, rd.y);
        float lit = pow(sunAmount, 3.0);
        vec3 cloudLit = mix(vec3(0.95, 0.5, 0.55), vec3(1.0, 0.75, 0.5), lit) * (0.8 + 2.2 * lit);
        vec3 cloudShade = vec3(0.36, 0.3, 0.45);
        col = mix(col, mix(cloudShade, cloudLit, 0.6 + 0.4 * lit), streak * 0.75);
        col = mix(col, mix(cloudShade * 0.9, cloudLit, 0.35 + 0.65 * lit), puff * 0.85);
    }
    return col;
}

// ---------------------------------------------------------------- rocks

// Sea-stack column: tapered, eroded, with horizontal strata.
float stack(vec3 p, vec2 c, float r, float h)
{
    vec3 q = p - vec3(c.x, 0.0, c.y);
    q.x -= 0.04 * q.y * sin(c.x * 3.1);
    q.xz = rot2(c.x * 0.7) * q.xz;
    float taper = r * (1.0 - 0.22 * clamp(q.y / h, 0.0, 1.0));
    float wobble = 0.3 * sin(q.y * 0.6 + c.x) * r * 0.25;
    float d = length(q.xz * vec2(1.0, 1.35)) / 1.15 - taper - wobble;
    d = max(d, q.y - h + 0.08 * dot(q.xz, q.xz));
    return d;
}

float rockDetail(vec3 p)
{
    float strata = sin(p.y * 2.1 + fbm3(p * 0.25, 2) * 5.0);
    strata = smoothstep(0.1, 0.95, strata) * 0.16;
    return fbm3(p * 0.45, 3) * 1.5 + fbm3(p * 1.9, 2) * 0.22 + strata;
}

// Signed distance to the headland, sea stacks and arch.
float mapRocks(vec3 p)
{
    // Headland on the left: a cliff wall with a grassy plateau.
    float cliff = p.x + 30.0 + 6.0 * sin(p.z * 0.07) + 3.0 * sin(p.z * 0.19 + 1.0) - 0.25 * p.y;
    cliff = max(cliff, p.y - 14.0 - 3.0 * sin(p.z * 0.09) - 0.08 * (p.x + 30.0));
    cliff = max(cliff, -p.z - 150.0);

    // Stacks marching out from the headland.
    float d = cliff;
    d = min(d, stack(p, vec2(-12.0, -42.0), 4.2, 15.0));
    d = min(d, stack(p, vec2(-2.0, -58.0), 3.0, 11.0));
    d = min(d, stack(p, vec2(9.0, -70.0), 3.6, 13.0));
    d = min(d, stack(p, vec2(3.5, -36.0), 1.4, 4.5));

    // A chunky arch, set side-on toward the rising sun.
    vec3 a = p - vec3(24.0, 0.0, -62.0);
    a.xz = rot2(-0.5) * a.xz;
    vec2 tq = vec2(length(a.xy) - 9.0, a.z);
    float arch = max(abs(tq.x) - 2.4, abs(tq.y) - 3.0);
    arch = max(arch, -a.y - 1.0);
    d = min(d, arch);

    // Erosion only where it matters, so far rays stay cheap.
    if (d < 4.0)
        d += rockDetail(p) - 1.0;
    return d;
}

vec3 rockNormal(vec3 p)
{
    vec2 e = vec2(0.03, 0.0);
    return normalize(vec3(
        mapRocks(p + e.xyy) - mapRocks(p - e.xyy),
        mapRocks(p + e.yxy) - mapRocks(p - e.yxy),
        mapRocks(p + e.yyx) - mapRocks(p - e.yyx)));
}

float marchRocks(vec3 ro, vec3 rd, float maxT, int steps)
{
    float t = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        vec3 p = ro + rd * t;
        float d = mapRocks(p);
        if (d < 0.002 * t)
            return t;
        t += d * 0.7;
        if (t > maxT)
            break;
    }
    return -1.0;
}

float softShadow(vec3 ro, vec3 rd)
{
    float res = 1.0;
    float t = 0.3;
    for (int i = 0; i < 28; ++i)
    {
        float h = mapRocks(ro + rd * t);
        res = min(res, 6.0 * h / t);
        t += clamp(h, 0.4, 4.0);
        if (res < 0.02 || t > 60.0)
            break;
    }
    return clamp(res, 0.0, 1.0);
}

// ---------------------------------------------------------------- water

// Sum of directional sine waves with analytic slope; returns (height, dh/dx, dh/dz).
vec3 waves(vec2 p, float t, int count)
{
    vec3 h = vec3(0.0);
    float amp = 0.12;
    float freq = 0.32;
    float speed = 0.8;
    float angle = 1.35;
    // Gentle domain warp keeps the wave train from looking like a lattice.
    p += vec2(noise2(p * 0.05), noise2(p * 0.05 + 7.3)) * 6.0;
    for (int i = 0; i < count; ++i)
    {
        vec2 dir = vec2(cos(angle), sin(angle));
        float ph = dot(p, dir) * freq - t * speed + hash11(float(i)) * TAU;
        h.x += amp * sin(ph);
        h.yz += amp * freq * cos(ph) * dir;
        amp *= 0.56;
        freq *= 1.47;
        speed *= 1.2;
        angle += 2.399;
    }
    return h;
}

float swell(vec2 p, float t)
{
    return waves(p, t, 3).x;
}

vec3 waterNormal(vec2 p, float t, float dist)
{
    // Fewer, broader waves in the distance avoid shimmer.
    int count = dist < 25.0 ? 12 : (dist < 70.0 ? 8 : 5);
    vec3 h = waves(p, t, count);
    return normalize(vec3(-h.y, 1.0, -h.z));
}

// ---------------------------------------------------------------- shading

vec3 shadeRock(vec3 p, vec3 rd)
{
    vec3 n = rockNormal(p);
    float grass = smoothstep(0.55, 0.85, n.y) * smoothstep(12.0, 15.5, p.y);
    float wet = 1.0 - smoothstep(0.0, 1.4 + 0.4 * sin(p.x * 0.7 + p.z), p.y);
    vec3 albedo = mix(vec3(0.42, 0.37, 0.33), vec3(0.66, 0.6, 0.52), fbm3(p * 1.7, 2));
    albedo *= 0.82 + 0.22 * sin(p.y * 2.1 + fbm3(p * 0.25, 2) * 5.0);
    albedo = mix(albedo, vec3(0.22, 0.32, 0.12), grass);
    albedo = mix(albedo, albedo * 0.35, wet);

    float ndl = max(dot(n, SUN_DIR), 0.0);
    float sh = ndl > 0.0 ? softShadow(p + n * 0.1, SUN_DIR) : 0.0;
    vec3 sky = vec3(0.42, 0.38, 0.56) * (0.45 + 0.55 * n.y);
    vec3 bounce = vec3(0.6, 0.42, 0.35) * clamp(-n.y, 0.0, 1.0) * 0.6;
    vec3 col = albedo * (SUN_COL * 0.3 * ndl * sh + sky * 0.7 + bounce);
    // Back-lit rim where the low sun grazes the silhouette.
    float rim = pow(1.0 - max(dot(n, -rd), 0.0), 4.0) * pow(max(dot(rd, SUN_DIR), 0.0), 4.0);
    col += vec3(1.0, 0.6, 0.4) * rim * 0.5;
    // Wet rock glints.
    vec3 h = normalize(SUN_DIR - rd);
    col += SUN_COL * 0.03 * pow(max(dot(n, h), 0.0), 40.0) * wet * sh;
    return col;
}

// Height-fog with drifting mist banks and warm in-scatter toward the sun.
vec3 applyMist(vec3 col, vec3 ro, vec3 rd, float dist, float t)
{
    float sunAmount = max(dot(rd, SUN_DIR), 0.0);
    vec3 fogCol = mix(vec3(0.46, 0.38, 0.52), vec3(1.0, 0.6, 0.4), pow(sunAmount, 12.0));
    // Analytic exponential height fog.
    float k = 0.09;
    float density = 0.006;
    float kd = k * rd.y;
    float path = abs(kd) < 1e-4 ? dist : (1.0 - exp(-kd * dist)) / kd;
    float fog = density * exp(-k * ro.y) * path;
    // Banks of mist hugging the water.
    vec3 mp = ro + rd * min(dist, 90.0) * 0.6;
    float bank = smoothstep(0.35, 0.8, fbm2(mp.xz * 0.04 + vec2(t * 0.02, 0.0), 4));
    fog *= 0.75 + 0.9 * bank;
    float a = 1.0 - exp(-fog);
    return mix(col, fogCol, clamp(a, 0.0, 1.0));
}

vec3 gulls(vec3 ro, vec3 rd, float sceneDist, float t)
{
    float cover = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        float fi = float(i);
        float phase = t * (0.09 + 0.02 * hash11(fi)) + fi * 1.7;
        vec3 c = vec3(-6.0 + 6.0 * sin(phase) + fi * 3.5,
            8.0 + 2.5 * sin(phase * 1.7 + fi) + fi * 1.2,
            -16.0 - 5.0 * cos(phase) - fi * 5.0);
        vec3 toC = c - ro;
        float along = dot(toC, rd);
        if (along < 0.0 || along > sceneDist)
            continue;
        // Billboard frame facing the camera.
        vec3 fwd = normalize(toC);
        vec3 right = normalize(cross(fwd, vec3(0.0, 1.0, 0.0)));
        vec3 up = cross(right, fwd);
        vec3 hit = ro + rd * (length(toC));
        vec2 q = vec2(dot(hit - c, right), dot(hit - c, up)) / (1.1 + 0.1 * fi);
        float flap = sin(t * (4.5 + fi) + fi * 2.0);
        flap = mix(0.6, flap, step(0.65, fract(t * 0.12 + fi * 0.3)));
        q = rot2(0.12 * sin(phase * 2.0)) * q;
        cover = max(cover, gullShape(q, flap));
    }
    return vec3(cover);
}

void main()
{
    float t = sceneTime();
    vec3 ro = cameraOrigin();
    vec3 rd = normalize(ray);

    float tWater = rd.y < 0.0 ? -ro.y / rd.y : 1e9;
    float tRock = marchRocks(ro, rd, 220.0, 140);
    bool hitRock = tRock > 0.0 && tRock < tWater;

    vec3 col;
    float dist;
    if (hitRock)
    {
        vec3 p = ro + rd * tRock;
        col = shadeRock(p, rd);
        dist = tRock;
    }
    else if (tWater < 1e8)
    {
        vec3 p = ro + rd * tWater;
        dist = tWater;
        vec3 n = waterNormal(p.xz, t, dist);
        vec3 rr = reflect(rd, n);
        rr.y = abs(rr.y);
        float fres = 0.02 + 0.98 * pow(1.0 - max(dot(n, -rd), 0.0), 5.0);

        vec3 refl = skyColor(rr, t);
        float tr = marchRocks(p + vec3(0.0, 0.05, 0.0), rr, 160.0, 60);
        if (tr > 0.0)
            refl = applyMist(shadeRock(p + rr * tr, rr), p, rr, tr, t);

        vec3 deep = vec3(0.03, 0.06, 0.1);
        // Sunlit swell picks up a little turquoise as light passes through crests.
        float sss = pow(max(dot(rd, SUN_DIR) * 0.5 + 0.5, 0.0), 3.0) * max(swell(p.xz, t) + 0.1, 0.0);
        vec3 body = deep + vec3(0.1, 0.35, 0.35) * sss * 2.0;
        col = mix(body, refl, fres);

        // Glittering sun path.
        vec3 h = normalize(SUN_DIR - rd);
        float spark = pow(max(dot(n, h), 0.0), 900.0);
        col += SUN_COL * spark * 6.0;

        // Lapping foam where the swell meets the rocks.
        float shore = mapRocks(vec3(p.x, 0.2, p.z));
        float foamLine = smoothstep(1.8, 0.0, shore) * (0.5 + 0.5 * sin(shore * 6.0 - t * 2.5));
        foamLine *= smoothstep(0.35, 0.65, fbm2(p.xz * 1.5 + t * 0.3, 3));
        col = mix(col, vec3(0.95, 0.85, 0.85) * 1.1, foamLine * 0.8);
    }
    else
    {
        col = skyColor(rd, t);
        dist = 1e4;
    }

    if (dist < 1e4)
        col = applyMist(col, ro, rd, dist, t);

    vec3 bird = gulls(ro, rd, dist, t);
    col = mix(col, vec3(0.16, 0.12, 0.16), bird.x * 0.9);

    fragColor = vec4(col, 1.0);
}
