#include "default_parameters.h"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

// NYX bridge instruments share this live-reloaded procedural signal language.
#define PI 3.14159265359

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    return fract(p * (p + p));
}

float hash21(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

mat2 rotate2(float a)
{
    float c = cos(a), s = sin(a);
    return mat2(c, -s, s, c);
}

float lineGlow(float distanceToLine, float width)
{
    return exp(-abs(distanceToLine) / max(width, 0.0001));
}

float ringGlow(float radius, float target, float width)
{
    return lineGlow(abs(radius - target), width);
}

float sdBox(vec2 p, vec2 b)
{
    vec2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

float sdOctahedron(vec3 p, float size)
{
    p = abs(p);
    return (p.x + p.y + p.z - size) * 0.57735027;
}

float sdTorus(vec3 p, vec2 radii)
{
    vec2 q = vec2(length(p.xz) - radii.x, p.y);
    return length(q) - radii.y;
}

float geometryMap(vec3 p, float t, int mode)
{
    if (mode == 0) {
        p.xz = rotate2(t * 0.72) * p.xz;
        p.xy = rotate2(t * 0.43) * p.xy;
        float crystal = abs(sdOctahedron(p, 0.78)) - 0.026;
        vec3 core = p;
        core.yz = rotate2(PI * 0.5) * core.yz;
        float torus = sdTorus(core, vec2(0.43, 0.052));
        return min(crystal, torus);
    }

    vec3 a = p;
    a.xz = rotate2(t * 0.36) * a.xz;
    float ringA = sdTorus(a, vec2(0.66, 0.032));
    vec3 b = p;
    b.xy = rotate2(t * -0.48 + 0.9) * b.xy;
    b.yz = rotate2(PI * 0.5) * b.yz;
    float ringB = sdTorus(b, vec2(0.53, 0.028));
    vec3 c = p;
    c.yz = rotate2(t * 0.61 + 1.7) * c.yz;
    c.xy = rotate2(PI * 0.5) * c.xy;
    float ringC = sdTorus(c, vec2(0.39, 0.024));
    return min(ringA, min(ringB, ringC));
}

vec3 geometryNormal(vec3 p, float t, int mode)
{
    const float e = 0.002;
    vec2 h = vec2(e, 0.0);
    return normalize(vec3(
        geometryMap(p + h.xyy, t, mode) - geometryMap(p - h.xyy, t, mode),
        geometryMap(p + h.yxy, t, mode) - geometryMap(p - h.yxy, t, mode),
        geometryMap(p + h.yyx, t, mode) - geometryMap(p - h.yyx, t, mode)));
}

vec3 hologramGeometry(vec2 p, float t, int mode)
{
    vec3 ro = vec3(0.0, 0.0, 2.75);
    vec3 rd = normalize(vec3(p * 1.05, -2.35));
    float travel = 0.0;
    float nearest = 10.0;
    vec3 position = ro;
    bool hit = false;
    for (int i = 0; i < 72; ++i) {
        position = ro + rd * travel;
        float d = geometryMap(position, t, mode);
        nearest = min(nearest, abs(d));
        if (abs(d) < 0.0015) {
            hit = true;
            break;
        }
        travel += d * 0.72;
        if (travel > 6.0) break;
    }

    vec3 cyan = vec3(0.02, 0.85, 1.8);
    vec3 magenta = vec3(1.55, 0.04, 0.82);
    vec3 tint = mix(cyan, magenta,
        0.5 + 0.5 * sin(position.y * 8.0 + position.x * 5.0 + t));
    vec3 col = tint * exp(-nearest * 13.0) * 0.22;
    if (hit) {
        vec3 normal = geometryNormal(position, t, mode);
        float diffuse = max(dot(normal, normalize(vec3(-0.4, 0.8, 0.7))), 0.0);
        float fresnel = pow(1.0 - max(dot(normal, -rd), 0.0), 2.2);
        float circuitry = 0.35 + 0.65 * smoothstep(0.72, 0.98,
            sin((position.x + position.y - position.z) * 32.0 - t * 5.0));
        col += tint * (0.18 + diffuse * 0.55 + fresnel * 1.7 + circuitry * 0.22);
    }
    return col;
}

float hexDistance(vec2 p)
{
    p = abs(p);
    return max(dot(p, normalize(vec2(1.0, 1.7320508))), p.x);
}

vec4 hexTile(vec2 p)
{
    // Two interleaved rectangular lattices form a triangular lattice. Picking
    // the nearer center gives each point one exact hexagonal Voronoi cell, so
    // adjacent outlines share the same boundary without row-offset seams.
    const vec2 lattice = vec2(1.0, 1.7320508);
    vec2 halfLattice = lattice * 0.5;
    vec2 localA = mod(p, lattice) - halfLattice;
    vec2 localB = mod(p - halfLattice, lattice) - halfLattice;
    vec2 local = dot(localA, localA) < dot(localB, localB) ? localA : localB;
    return vec4(local, p - local);
}

vec3 palette(float x)
{
    vec3 a = vec3(0.04, 0.15, 0.24);
    vec3 b = vec3(0.22, 0.58, 0.62);
    vec3 c = vec3(0.82, 1.00, 1.00);
    vec3 d = vec3(0.04, 0.18, 0.34);
    return a + b * cos(6.28318 * (c * x + d));
}

vec3 reactor(vec2 p, float t)
{
    float r = length(p);
    float a = atan(p.y, p.x);
    vec3 col = vec3(0.004, 0.012, 0.030);
    for (int i = 0; i < 7; ++i) {
        float fi = float(i);
        float rr = 0.16 + fi * 0.105 + 0.018 * sin(t * (1.2 + fi * 0.09) + fi);
        float arc = 0.32 + 0.68 * smoothstep(-0.5, 0.8, sin(a * (3.0 + mod(fi, 3.0)) + t * (mod(fi, 2.0) < 1.0 ? 1.0 : -1.0) + fi));
        col += mix(vec3(0.02, 0.65, 1.5), vec3(1.2, 0.05, 0.85), fi / 7.0) * ringGlow(r, rr, 0.008) * arc;
    }
    col += vec3(0.3, 0.8, 1.8) * exp(-r * 8.0) * (1.2 + 0.3 * sin(t * 4.0));
    col += vec3(0.6, 0.1, 1.4) * lineGlow(abs(sin(a * 8.0 + t * 2.0)) * r, 0.012) * smoothstep(0.8, 0.15, r);
    return col;
}

vec3 radar(vec2 p, float t)
{
    float r = length(p);
    float a = atan(p.y, p.x);
    float sweep = mod(t * 0.8, 2.0 * PI) - PI;
    float delta = abs(atan(sin(a - sweep), cos(a - sweep)));
    vec3 col = vec3(0.003, 0.018, 0.022);
    col += vec3(0.02, 0.5, 0.48) * (ringGlow(r, 0.25, 0.006) + ringGlow(r, 0.5, 0.006) + ringGlow(r, 0.75, 0.006));
    col += vec3(0.04, 1.2, 0.8) * exp(-delta * 10.0) * smoothstep(0.94, 0.05, r);
    col += vec3(0.02, 0.30, 0.24) * lineGlow(p.x, 0.004) + vec3(0.02, 0.30, 0.24) * lineGlow(p.y, 0.004);
    for (int i = 0; i < 9; ++i) {
        float fi = float(i);
        vec2 b = vec2(sin(fi * 17.7) * 0.68, cos(fi * 11.3) * 0.68);
        float pulse = 0.55 + 0.45 * sin(t * 3.0 + fi * 1.7);
        col += mix(vec3(0.0, 1.4, 0.8), vec3(1.6, 0.18, 0.35), step(0.72, hash11(fi))) * ringGlow(length(p - b), 0.025, 0.009) * pulse;
    }
    return col;
}

vec3 signals(vec2 p, float t)
{
    vec3 col = vec3(0.005, 0.009, 0.025);
    vec2 g = fract((p + 4.0) * 6.0) - 0.5;
    col += vec3(0.012, 0.045, 0.10) * (lineGlow(g.x, 0.010) + lineGlow(g.y, 0.010));
    col += hologramGeometry(p * 1.13, t, 0);
    float upper = 0.72 + 0.045 * sin(p.x * 18.0 + t * 3.1);
    float lower = -0.72 + 0.045 * sin(p.x * 15.0 - t * 2.7);
    col += vec3(0.05, 1.2, 1.8) * lineGlow(p.y - upper, 0.009);
    col += vec3(1.5, 0.06, 0.85) * lineGlow(p.y - lower, 0.009);
    float pulse = mod(t * 0.55, 2.4) - 1.2;
    col += vec3(0.35, 0.85, 1.5) * lineGlow(p.x - pulse, 0.006) * 0.55;
    col += vec3(0.15, 0.8, 1.3) * ringGlow(length(p), 0.92, 0.009) * 0.7;
    return col;
}

vec3 flightStars(vec2 p, float t)
{
    vec3 col = vec3(0.0);
    for (int i = 0; i < 160; ++i) {
        float fi = float(i);
        float travel = t * (0.34 + 0.036 * hash11(fi * 7.31))
            + hash11(fi * 19.17);
        float cycle = floor(travel);
        float depth = fract(travel);
        float angle = 2.0 * PI * hash11(fi * 31.73 + cycle * 13.11);
        vec2 direction = vec2(cos(angle), sin(angle));
        float radius = 0.018 + 1.78 * depth * depth;
        vec2 head = direction * radius;
        float tailLength = mix(0.006, 0.19, depth * depth);
        vec2 tail = head - direction * tailLength;
        vec2 segment = head - tail;
        float along = clamp(dot(p - tail, segment)
            / max(dot(segment, segment), 0.00001), 0.0, 1.0);
        float distanceToTrail = length(p - (tail + segment * along));
        float streak = exp(-distanceToTrail * mix(230.0, 105.0, depth))
            * smoothstep(0.0, 0.82, along) * depth * depth;
        float core = exp(-length(p - head) * mix(260.0, 120.0, depth));
        vec3 starColor = mix(vec3(0.34, 0.65, 1.25), vec3(1.15, 1.35, 1.55),
            hash11(fi * 5.93));
        col += starColor * (streak * 1.45 + core * (0.28 + depth * 1.8));
    }
    return col;
}

vec3 astrogation(vec2 p, float t)
{
    vec3 col = vec3(0.003, 0.008, 0.025) + flightStars(p, t);
    vec2 q = p * rotate2(0.16 + t * 0.035);
    col += hologramGeometry(p * 1.08, t, 1) * 1.15;
    for (int i = 0; i < 5; ++i) {
        float fi = float(i);
        vec2 o = vec2(-0.25 + fi * 0.125, 0.14 * sin(fi * 2.0 + t * 0.24));
        vec2 z = q - o;
        z.y *= 1.65 + 0.22 * fi;
        float orbit = ringGlow(length(z), 0.22 + fi * 0.055, 0.004);
        col += mix(vec3(0.04, 0.5, 1.1), vec3(0.8, 0.18, 1.25), fi / 5.0) * orbit;
        float phase = t * (0.48 + fi * 0.07) + fi;
        vec2 body = o + vec2(cos(phase), sin(phase) / (1.65 + 0.22 * fi)) * (0.22 + fi * 0.055);
        col += vec3(0.55, 1.15, 1.8) * exp(-length(q - body) * 52.0);
    }
    return col;
}

vec3 powerGrid(vec2 p, float t)
{
    vec3 col = vec3(0.006, 0.012, 0.022);
    float lane = floor((p.x + 1.0) * 5.0);
    float x = fract((p.x + 1.0) * 5.0) - 0.5;
    float level = 0.28 + 0.58 * (0.5 + 0.5 * sin(t * (0.7 + lane * 0.12) + lane * 1.9));
    float segment = step(0.18, abs(fract((p.y + 1.0) * 14.0) - 0.5));
    float filled = step(p.y, -1.0 + level * 2.0);
    vec3 hot = mix(vec3(0.0, 0.75, 1.5), vec3(1.6, 0.08, 0.35), smoothstep(0.55, 0.92, level));
    col += hot * filled * segment * smoothstep(0.46, 0.34, abs(x)) * 0.7;
    col += hot * lineGlow(abs(x) - 0.38, 0.012) * 0.32;
    float bus = lineGlow(p.y + 0.82, 0.012) + lineGlow(p.y - 0.82, 0.012);
    col += vec3(0.2, 0.8, 1.4) * bus;
    return col;
}

vec3 singularity(vec2 p, float t)
{
    vec2 q = p * rotate2(-0.28);
    q.y *= 2.5;
    float r = length(q);
    float a = atan(q.y, q.x);
    vec3 col = vec3(0.002, 0.003, 0.012);
    float disk = ringGlow(r, 0.48 + 0.035 * sin(a * 9.0 - t * 2.0), 0.025);
    vec3 diskCol = mix(vec3(1.6, 0.08, 0.65), vec3(0.08, 0.75, 1.8), 0.5 + 0.5 * sin(a - t));
    col += diskCol * disk * (1.0 + 0.4 * sin(a * 16.0 + t * 3.0));
    col += vec3(0.35, 0.08, 0.85) * ringGlow(r, 0.34, 0.018);
    col *= smoothstep(0.28, 0.36, r);
    col += vec3(0.1, 0.32, 0.8) * exp(-abs(q.x) * 18.0) * smoothstep(0.34, 0.8, abs(q.y));
    return col;
}

vec3 shield(vec2 p, float t)
{
    vec4 tile = hexTile(p * 4.2);
    vec2 f = tile.xy;
    vec2 cell = tile.zw;
    float edge = lineGlow(hexDistance(f) - 0.5, 0.025);
    float pulse = 0.24 + 0.76 * pow(0.5 + 0.5 * sin(t * 2.0 - length(cell) * 0.8), 6.0);
    float fault = step(0.84, hash21(cell));
    vec3 col = vec3(0.003, 0.014, 0.025);
    col += mix(vec3(0.02, 0.55, 1.5), vec3(1.4, 0.05, 0.55), fault) * edge * pulse;
    float wave = ringGlow(length(p), mod(t * 0.34, 1.4), 0.025);
    col += vec3(0.12, 0.9, 1.6) * wave * edge * 1.7;
    return col;
}

vec3 targeting(vec2 p, float t)
{
    vec2 target = vec2(0.28 * sin(t * 0.73), 0.22 * cos(t * 0.91));
    vec2 q = p - target;
    float r = length(q);
    float a = atan(q.y, q.x);
    vec3 col = vec3(0.009, 0.008, 0.021);
    float broken = smoothstep(0.15, 0.75, sin(a * 8.0 + t * 1.3));
    col += vec3(1.5, 0.10, 0.55) * ringGlow(r, 0.27, 0.010) * broken;
    col += vec3(0.08, 0.85, 1.4) * ringGlow(r, 0.46, 0.008) * (1.0 - broken * 0.45);
    col += vec3(1.1, 0.14, 0.42) * (lineGlow(q.x, 0.006) * step(0.16, abs(q.y)) + lineGlow(q.y, 0.006) * step(0.16, abs(q.x)));
    vec2 boxp = q * rotate2(t * 0.22);
    col += vec3(0.2, 0.7, 1.4) * lineGlow(abs(sdBox(boxp, vec2(0.34))) - 0.005, 0.009);
    col += vec3(1.0, 0.18, 0.35) * exp(-r * 28.0) * (1.0 + sin(t * 7.0));
    return col;
}

float noise2(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}

float fbm(vec2 p)
{
    float value = 0.0, amplitude = 0.5;
    for (int i = 0; i < 5; ++i) {
        value += amplitude * noise2(p);
        p = rotate2(0.57) * p * 2.03 + 1.7;
        amplitude *= 0.5;
    }
    return value;
}

vec3 plasma(vec2 p, float t)
{
    vec2 q = p;
    q.x += 0.28 * sin(q.y * 2.4 + t * 0.8);
    q.y += 0.20 * cos(q.x * 2.8 - t * 0.6);
    float n = fbm(q * 2.2 + vec2(0.0, -t * 0.28));
    float river = exp(-abs(p.x - 0.28 * sin(p.y * 2.5 + t) - (n - 0.5) * 0.7) * 4.0);
    vec3 col = vec3(0.003, 0.006, 0.025);
    col += palette(n + t * 0.04) * river * 1.8;
    col += vec3(0.35, 0.05, 1.0) * pow(n, 4.0) * 0.7;
    for (int i = 0; i < 6; ++i) {
        float fi = float(i);
        vec2 s = vec2(0.42 * sin(fi * 7.0 + t * (0.25 + fi * 0.02)), mod(fi * 0.37 - t * 0.12, 2.2) - 1.1);
        col += vec3(0.2, 0.8, 1.5) * exp(-length(p - s) * 30.0);
    }
    return col;
}

vec3 corridor(vec2 p, float t)
{
    vec3 col = vec3(0.003, 0.009, 0.021);
    vec2 q = p;
    q.x += 0.12 * sin(t * 0.33);
    float z = abs(q.y) + 0.045;
    float perspectiveX = abs(fract(q.x / z * 0.28 + 0.5) - 0.5);
    float perspectiveY = abs(fract(1.2 / z - t * 1.2) - 0.5);
    float floorMask = smoothstep(-0.02, -0.75, q.y);
    col += vec3(0.02, 0.55, 1.2) * lineGlow(perspectiveX, 0.028) * floorMask;
    col += vec3(0.45, 0.08, 1.2) * lineGlow(perspectiveY, 0.05) * floorMask;
    float rails = lineGlow(abs(q.x) - (0.12 + abs(q.y) * 0.48), 0.012);
    col += vec3(0.1, 1.0, 1.7) * rails;
    col += vec3(0.8, 0.15, 1.2) * lineGlow(q.y, 0.012);
    float gate = sdBox(vec2(q.x, q.y - 0.25), vec2(0.44, 0.55));
    col += vec3(0.08, 0.5, 1.3) * lineGlow(abs(gate), 0.014) * 0.75;
    return col;
}

vec3 cryoCore(vec2 p, float t)
{
    float r = length(p);
    float a = atan(p.y, p.x);
    vec3 col = vec3(0.002, 0.012, 0.020);
    float blades = pow(abs(sin(a * 6.0 + t * 0.7)), 22.0) * smoothstep(0.72, 0.16, r);
    col += vec3(0.08, 0.75, 1.5) * blades;
    col += vec3(0.15, 1.1, 1.5) * ringGlow(r, 0.58, 0.011);
    col += vec3(0.9, 0.12, 1.0) * ringGlow(r, 0.38 + 0.025 * sin(t * 3.0), 0.016);
    float core = exp(-r * 12.0) * (1.4 + 0.5 * sin(t * 5.0));
    col += vec3(0.55, 1.2, 1.8) * core;
    for (int i = 0; i < 12; ++i) {
        float fi = float(i);
        float ang = fi / 12.0 * 2.0 * PI + t * 0.2;
        vec2 n = vec2(cos(ang), sin(ang)) * 0.82;
        col += vec3(0.05, 0.48, 1.1) * exp(-length(p - n) * 45.0);
    }
    return col;
}

vec3 instrument(vec2 p, float t)
{
#if PANEL_ID == 0
    return reactor(p, t);
#elif PANEL_ID == 1
    return radar(p, t);
#elif PANEL_ID == 2
    return signals(p, t);
#elif PANEL_ID == 3
    return astrogation(p, t);
#elif PANEL_ID == 4
    return powerGrid(p, t);
#elif PANEL_ID == 5
    return singularity(p, t);
#elif PANEL_ID == 6
    return shield(p, t);
#elif PANEL_ID == 7
    return targeting(p, t);
#elif PANEL_ID == 8
    return plasma(p, t);
#else
    return corridor(p, t) + cryoCore(p * 1.25, t) * 0.55;
#endif
}

void main()
{
    vec2 resolution = max(ubo.iResolution.xy, vec2(1.0));
    vec2 localFrag = uv * resolution;
    vec2 p = (2.0 * uv - 1.0) * resolution / min(resolution.x, resolution.y);
    float t = ubo.iTime * 2.0;
    vec3 col = instrument(p, t);

    // Shared glass, bezel, and alert-language treatment. Scan conversion and
    // analogue noise are applied by the scenegraph's full-resolution CRT pass.
    float vignette = smoothstep(1.35, 0.22, length((uv - 0.5) * vec2(1.0, 0.72)));
    float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    float bezel = lineGlow(edge - 0.018, 0.004);
    float corner = step(0.92, max(abs(uv.x * 2.0 - 1.0), abs(uv.y * 2.0 - 1.0)));
    col *= 0.55 + 0.45 * vignette * 1.0;
    col += vec3(0.02, 0.30, 0.55) * bezel;
    col += vec3(0.04, 0.18, 0.32) * corner * 0.16;
    col = 1.0 - exp(-col * 1.15);
    fragColor = vec4(pow(max(col, 0.0), vec3(0.86)), 1.0);
}
