#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"

// SENSORS // LONG RANGE: a holographic survey of an alien surface. The
// heightfield is raymarched and drawn as glowing contour lines and grid,
// revealed by a sweeping scan front, with tracked contacts hovering above.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 ray;
layout(location = 0) out vec4 fragColor;

float terrain(vec2 p)
{
    float h = fbm2(p * 0.22, 6) * 2.6;
    // Ridged mountains in the distance.
    float ridge = 1.0 - abs(noise2(p * 0.35 + 7.0) * 2.0 - 1.0);
    h += ridge * ridge * 0.9;
    // A crater the scan is centred on.
    float r = length(p - vec2(0.0, -4.0));
    h -= 0.9 * exp(-r * r * 0.18);
    h += 0.35 * exp(-pow(r - 2.6, 2.0) * 3.0);
    return h - 1.6;
}

vec3 contacts(int i, float t)
{
    float fi = float(i);
    vec2 base = vec2(sin(fi * 3.1 + t * 0.05) * 4.5, -4.0 + cos(fi * 2.3 + t * 0.04) * 4.0);
    return vec3(base.x, terrain(base) + 0.7 + 0.2 * sin(t + fi), base.y);
}

void main()
{
    float t = sceneTime();
    vec3 rd = normalize(ray);
    vec3 ro = cameraOrigin();
    vec3 green = vec3(0.25, 1.0, 0.55);
    vec3 amber = vec3(1.0, 0.65, 0.2);

    // Faint volumetric haze background.
    vec3 col = vec3(0.0, 0.012, 0.01) + green * 0.02 * smoothstep(-0.2, 0.3, rd.y) * (1.0 - smoothstep(0.3, 0.9, rd.y));
    col += starField(rd, 0.08) * 0.35 * smoothstep(0.0, 0.2, rd.y);

    // Heightfield march with adaptive steps.
    float travelled = 0.0;
    float hitT = -1.0;
    float prevH = 0.0;
    float prevT = 0.0;
    for (int i = 0; i < 160; ++i)
    {
        vec3 p = ro + rd * travelled;
        float h = p.y - terrain(p.xz);
        if (h < 0.002 * travelled)
        {
            // Refine between the last two samples.
            hitT = mix(prevT, travelled, prevH / max(prevH - h, 1e-4));
            break;
        }
        prevH = h;
        prevT = travelled;
        travelled += max(h * 0.45, 0.01 + travelled * 0.002);
        if (travelled > 45.0)
            break;
    }

    // Sweep front moves away from the viewer and wraps.
    float sweepZ = 2.0 - fract(t * 0.07) * 20.0;

    if (hitT > 0.0)
    {
        vec3 p = ro + rd * hitT;
        float h = terrain(p.xz);
        vec2 e = vec2(0.02, 0.0);
        vec3 n = normalize(vec3(terrain(p.xz - e.xy) - terrain(p.xz + e.xy), 2.0 * e.x,
            terrain(p.xz - e.yx) - terrain(p.xz + e.yx)));

        // Contour lines: distance to the nearest iso-height, in screen pixels.
        float ch = h * 5.0;
        float contour = smoothstep(fwidth(ch) * 1.2, 0.0, abs(fract(ch + 0.5) - 0.5));
        float major = smoothstep(fwidth(h) * 1.5, 0.0, abs(fract(h + 0.5) - 0.5));

        // Survey grid.
        vec2 g = p.xz;
        vec2 gw = fwidth(g) * 1.2;
        vec2 gd = abs(fract(g + 0.5) - 0.5);
        float grid = max(smoothstep(gw.x, 0.0, gd.x), smoothstep(gw.y, 0.0, gd.y));

        // The front sweeps away from the viewer; terrain it has passed fades
        // with age, terrain ahead shows only last cycle's residual image.
        float front = p.z - sweepZ;
        float memory = front > 0.0 ? 0.25 + 0.75 * exp(-front * 0.12) : 0.25;
        float fresh = exp(-abs(front) * 6.0);

        float relief = 0.25 + 0.75 * max(dot(n, normalize(vec3(-0.4, 0.8, 0.3))), 0.0);
        vec3 surf = green * (contour * 0.45 + major * 1.1 + grid * 0.2) * memory;
        surf += green * 0.035 * relief * memory;
        // Height-coded tint: peaks run warm.
        surf = mix(surf, surf * amber * 1.6, smoothstep(0.4, 1.6, h));
        surf += vec3(0.6, 1.0, 0.8) * fresh * (0.3 + 1.6 * contour + 0.8 * grid);
        float fog = exp(-hitT * 0.045);
        col = mix(col, surf, fog);
        col += green * fresh * 0.15 * (1.0 - fog);
    }

    // Vertical scan curtain.
    if (abs(rd.z) > 1e-4)
    {
        float tc = (sweepZ - ro.z) / rd.z;
        if (tc > 0.0 && (hitT < 0.0 || tc < hitT))
        {
            vec3 p = ro + rd * tc;
            float above = p.y - terrain(p.xz);
            float curtain = exp(-max(above, 0.0) * 1.2) * smoothstep(12.0, 6.0, abs(p.x));
            float lines = 0.5 + 0.5 * sin(p.y * 40.0 - t * 6.0);
            col += green * curtain * (0.08 + 0.06 * lines);
        }
    }

    // Tracked contacts: diamond markers with altitude tethers.
    for (int i = 0; i < 6; ++i)
    {
        vec3 c = contacts(i, t);
        vec3 oc = c - ro;
        float tc = dot(oc, rd);
        if (tc <= 0.0 || (hitT > 0.0 && tc > hitT + 0.5))
            continue;
        vec3 closest = ro + rd * tc;
        vec3 d3 = closest - c;
        float d = length(d3);
        bool hostile = (i == 1 || i == 4);
        vec3 cc = hostile ? vec3(1.0, 0.25, 0.15) : amber;
        float blink = hostile ? (0.6 + 0.4 * step(0.5, fract(t * 2.0 + float(i)))) : 1.0;
        float size = 0.022 * tc;
        float diamond = abs(d3.x) + abs(d3.y);
        float marker = smoothstep(size * 0.25, 0.0, abs(diamond - size * 2.0));
        col += cc * (exp(-d / (0.01 * tc)) * 1.8 + marker * 1.2) * blink;

        // Tether: vertical line from contact down to the ground.
        float ground = terrain(c.xz);
        vec3 a0 = vec3(c.x, ground, c.z);
        vec3 ba = c - a0;
        vec3 oa = ro - a0;
        float rb = dot(rd, ba);
        float bb = dot(ba, ba);
        float s = clamp((dot(oa, ba) - rb * dot(oa, rd)) / max(bb - rb * rb, 1e-5), 0.0, 1.0);
        vec3 pa = a0 + ba * s;
        float tt = dot(pa - ro, rd);
        float dl = length(ro + rd * tt - pa);
        float dash = step(0.5, fract(s * 8.0 - t));
        col += cc * smoothstep(0.004 * tt, 0.0, dl) * dash * 0.7;
    }

    fragColor = vec4(col, 1.0);
}
