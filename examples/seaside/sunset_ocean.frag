#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "sunset_sky.glsl"
#include "waves.glsl"
#include "boat_motion.glsl"

// SUNSET ocean: Fresnel sky reflection, back-lit crest translucency, a
// glittering sun path, crest foam and the yacht's wake.
layout(location = 0) in vec3 worldPos;
layout(location = 1) in vec2 gridXZ;
layout(location = 0) out vec4 fragColor;

float wakeFoam(vec2 xz, float t)
{
    vec2 rel = xz;
    float behind = dot(rel, -BOAT_FWD); // metres aft of the boat centre
    float lateral = dot(rel, BOAT_RIGHT);
    float fromBow = behind + 4.3;
    if (fromBow < 0.0)
        return 0.0;
    // Kelvin arms at ~19.5 degrees, fading downstream.
    float arm = abs(abs(lateral) - fromBow * 0.354 - 1.0);
    float kelvin = exp(-arm * 1.6) * exp(-fromBow * 0.03) * smoothstep(0.0, 3.0, fromBow);
    // Hull-side bow wave.
    float hull = exp(-abs(abs(lateral) - 1.25) * 3.0) * smoothstep(0.0, 1.0, fromBow) * smoothstep(9.0, 3.0, fromBow);
    // Churned propeller-and-keel trail straight behind the transom.
    float trail = exp(-abs(lateral) * (0.9 - 0.4 * smoothstep(4.0, 60.0, behind))) * smoothstep(3.5, 5.0, behind) * exp(-behind * 0.025);
    vec2 flow = waveSample(xz, t);
    float tex = fbm2(flow * 0.9, 4);
    float lacy = smoothstep(0.42, 0.7, tex + 0.15 * fbm2(flow * 4.0, 3));
    return clamp((kelvin * 0.8 + hull + trail) * lacy * 1.6, 0.0, 1.0);
}

void main()
{
    float t = sceneTime();
    vec3 cam = cameraOrigin();
    vec3 toCam = cam - worldPos;
    float dist = length(toCam);
    vec3 v = toCam / dist;
    vec3 rd = -v;

    float detail = 0.7 / (1.0 + dist * 0.06);
    vec4 nj = gerstnerNormal(gridXZ, t, detail);
    vec3 n = normalize(mix(nj.xyz, vec3(0.0, 1.0, 0.0), smoothstep(80.0, 900.0, dist) * 0.65));

    vec3 r = reflect(rd, n);
    r.y = abs(r.y);
    float fres = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    vec3 refl = sunsetSky(r, t);
    vec3 islandCol;
    float isl = islands(r, islandCol);
    refl = mix(refl, islandCol * 0.8, isl);

    // Body colour with light leaking through crests toward the viewer.
    float crest = clamp(worldPos.y * 1.6 + 0.4, 0.0, 1.0);
    float toward = pow(max(dot(rd, SUN_DIR) * 0.5 + 0.5, 0.0), 6.0);
    vec3 body = vec3(0.01, 0.018, 0.04) + vec3(0.3, 0.28, 0.1) * toward * crest * crest * 0.8;
    vec3 col = mix(body, refl, fres);

    // Sun glitter: a tight core plus a broad sheen.
    vec3 h = normalize(SUN_DIR + v);
    float nh = max(dot(n, h), 0.0);
    col += SUN_COL * (pow(nh, 1400.0) * 9.0 + pow(nh, 160.0) * 0.18);

    // Foam: compressed crests and the wake.
    vec2 flow = waveSample(gridXZ, t);
    float crestFoam = smoothstep(0.72, 0.95, nj.w + 0.25 * fbm2(flow * 0.6, 3)) * detail;
    float foam = max(crestFoam * 0.7, wakeFoam(gridXZ, t));
    vec3 foamCol = vec3(0.85, 0.55, 0.5) * (0.35 + 0.6 * max(dot(n, SUN_DIR), 0.0)) + vec3(0.12, 0.07, 0.12);
    col = mix(col, foamCol, foam);

    col = seaHaze(col, rd, dist);
    fragColor = vec4(col, 1.0);
}
