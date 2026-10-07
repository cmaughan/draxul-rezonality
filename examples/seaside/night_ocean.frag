#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "night_common.glsl"
#include "waves.glsl"

// NIGHT sea: moon glitter, starlit reflections and bioluminescent surf that
// flares where the swell breaks on the headland.
layout(location = 0) in vec3 worldPos;
layout(location = 1) in vec2 gridXZ;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 linearDepth;

void main()
{
    float t = sceneTime();
    vec3 cam = cameraOrigin();
    vec3 toCam = cam - worldPos;
    float dist = length(toCam);
    vec3 v = toCam / dist;
    vec3 rd = -v;

    float detail = 1.0 / (1.0 + dist * 0.04);
    vec4 nj = gerstnerNormal(gridXZ, t, detail);
    vec3 n = normalize(mix(nj.xyz, vec3(0.0, 1.0, 0.0), smoothstep(80.0, 900.0, dist) * 0.6));

    vec3 r = reflect(rd, n);
    r.y = abs(r.y);
    float fres = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    vec3 refl = nightSkyBase(r) + nightStars(r, t) * 0.4 * smoothstep(0.03, 0.2, r.y) * detail;
    vec3 col = mix(vec3(0.002, 0.006, 0.012), refl, fres);

    // Moon glitter path.
    vec3 h = normalize(MOON_DIR + v);
    float nh = max(dot(n, h), 0.0);
    col += MOON_COL * (pow(nh, 1600.0) * 8.0 + pow(nh, 220.0) * 0.12);

    // The lighthouse lamp's warm reflection.
    vec3 toLamp = normalize(LAMP_POS - worldPos);
    float lampSpec = pow(max(dot(n, normalize(toLamp + v)), 0.0), 600.0);
    col += LAMP_COL * lampSpec * 3.0;

    // Surf: pulses run shoreward and light up blue where they break.
    float shore = shoreDistance(gridXZ);
    vec2 flow = gridXZ * 0.35;
    float surge = sin(shore * 1.3 + t * 1.6 + fbm2(flow, 3) * 3.0);
    float zone = smoothstep(7.0, 0.5, shore) * smoothstep(-1.0, 0.5, shore);
    float breaker = smoothstep(0.35, 1.0, surge) * zone;
    float lace = smoothstep(0.4, 0.75, fbm2(gridXZ * 1.1 + vec2(t * 0.3, -t * 0.2), 4));
    float foam = clamp(breaker * (0.4 + 0.8 * lace) + zone * zone * lace * 0.35, 0.0, 1.0);
    float sparkle = 0.6 + 0.4 * sin(t * 6.0 + hash12(floor(gridXZ * 6.0)) * 30.0);
    col = mix(col, vec3(0.05, 0.07, 0.1), foam * 0.6);
    col += BIO_COL * foam * (0.9 + 1.6 * breaker) * sparkle;
    // Scattered sparks of plankton in the open water.
    float spark = step(0.995, hash12(floor(gridXZ * 3.0 + floor(t * 2.0) * 7.0))) * smoothstep(40.0, 5.0, shore);
    col += BIO_COL * spark * 2.0 * detail;

    col = nightHaze(col, rd, dist);
    fragColor = vec4(col, 1.0);
    linearDepth = vec4(dist);
}
