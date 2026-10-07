#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "kelp_common.glsl"

// MIDDAY backdrop: open water and the rippling surface overhead, with Snell's
// window showing the bright sky and the sun.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 ray;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 linearDepth;

void main()
{
    float t = sceneTime();
    vec3 ro = cameraOrigin();
    vec3 rd = normalize(ray);
    vec3 col = waterColor(rd);
    if (rd.y > 0.0)
    {
        float dist = (SURFACE_Y - ro.y) / rd.y;
        vec3 s = ro + rd * dist;
        // Rippled surface normal tilts the window edge and the sun.
        vec2 w = s.xz * 0.35 + vec2(t * 0.3, t * 0.2);
        vec2 slope = vec2(fbm2(w, 4) - fbm2(w + vec2(0.4, 0.0), 4), fbm2(w + 5.0, 4) - fbm2(w + vec2(5.4, 5.0), 4));
        vec3 dir = normalize(rd + vec3(slope.x, 0.0, slope.y) * 0.6);
        float cosCrit = 0.66; // cos(48.6 deg)
        float inWindow = smoothstep(cosCrit - 0.03, cosCrit + 0.03, dir.y);
        vec3 sky = mix(vec3(0.55, 0.8, 0.85), vec3(0.85, 0.97, 1.0), smoothstep(0.7, 1.0, dir.y)) * 1.4;
        float sun = pow(max(dot(dir, -LIGHT_DIR), 0.0), 300.0) * 25.0 + pow(max(dot(dir, -LIGHT_DIR), 0.0), 20.0) * 1.2;
        vec3 window = sky + SUN_UNDER * sun;
        // Outside the window the surface mirrors the dim depths below.
        vec3 mirror = vec3(0.02, 0.12, 0.15) + vec3(0.1, 0.25, 0.25) * (0.5 + 0.5 * slope.x * 8.0);
        vec3 surf = mix(mirror, window, inWindow);
        // Bright wavy caustic lines on the underside of the surface.
        surf += vec3(0.4, 0.6, 0.55) * caustic(s.xz * 0.25, t * 0.4) * 0.6;
        col = underwater(surf, s, rd, dist);
    }
    fragColor = vec4(col, 1.0);
    linearDepth = vec4(1.0e4);
}
