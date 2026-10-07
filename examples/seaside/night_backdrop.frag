#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"
#include "night_common.glsl"

// NIGHT backdrop: stars, Milky Way, moon, a distant village shore and the far sea.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 ray;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 linearDepth;

void main()
{
    float t = sceneTime();
    vec3 ro = cameraOrigin();
    vec3 rd = normalize(ray);
    vec3 col;
    float az = atan(rd.x, -rd.z);
    // Low far shore to the left with a scatter of warm cottage lights.
    float shore = 0.012 + 0.008 * fbm2(vec2(az * 8.0, 2.0), 4);
    shore *= smoothstep(-1.2, -0.9, az) * smoothstep(0.2, -0.1, az);
    if (shore > 0.001 && rd.y >= -0.0004 && rd.y < shore)
    {
        col = vec3(0.006, 0.009, 0.018);
        vec2 cell = vec2(floor(az * 260.0), 0.0);
        float lightOn = step(0.82, hash12(cell));
        float lx = fract(az * 260.0);
        float ly = rd.y / max(shore, 1e-4);
        float l = lightOn * exp(-pow((lx - 0.5) * 6.0, 2.0)) * exp(-pow((ly - 0.25 - 0.4 * hash12(cell + 3.0)) * 9.0, 2.0));
        col += vec3(1.0, 0.65, 0.3) * l * 3.0;
    }
    else if (rd.y >= 0.0)
        col = nightSky(rd, t);
    else
    {
        float dist = ro.y / max(-rd.y, 1e-4);
        vec3 r = reflect(rd, vec3(0.0, 1.0, 0.0));
        float fres = 0.02 + 0.98 * pow(1.0 - max(-rd.y, 0.0), 5.0);
        col = mix(vec3(0.002, 0.005, 0.01), nightSkyBase(r), fres);
        col = nightHaze(col, rd, dist);
    }
    fragColor = vec4(col, 1.0);
    linearDepth = vec4(1.0e4);
}
