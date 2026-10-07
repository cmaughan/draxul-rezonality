#version 450
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"
#include "common.glsl"

// Deep-space backdrop for the tactical view: nebula, stars, a distant sun
// and a holographic tactical grid plane with expanding sensor rings.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 ray;
layout(location = 0) out vec4 fragColor;

const vec3 SUN_DIR = normalize(vec3(-0.6, 0.35, -0.7));

void main()
{
    float t = sceneTime();
    vec3 rd = normalize(ray);
    vec3 ro = cameraOrigin();

    vec3 col = vec3(0.004, 0.006, 0.014);
    col += nebula(rd, vec3(0.05, 0.22, 0.55), vec3(0.55, 0.08, 0.38), t);
    col += starField(rd, 0.16);

    // Distant star with a soft corona.
    float sd = max(dot(rd, SUN_DIR), 0.0);
    col += vec3(1.0, 0.75, 0.5) * (pow(sd, 1800.0) * 40.0 + pow(sd, 60.0) * 0.5 + pow(sd, 6.0) * 0.06);

    // Tactical grid plane below the ship.
    float planeY = -1.15;
    if (rd.y < -0.001)
    {
        float dist = (planeY - ro.y) / rd.y;
        vec3 p = ro + rd * dist;
        vec2 g = p.xz;
        vec2 cell = abs(fract(g * 1.0) - 0.5);
        vec2 fw = fwidth(g) * 1.2;
        float lines = max(smoothstep(fw.x, 0.0, 0.5 - cell.x), smoothstep(fw.y, 0.0, 0.5 - cell.y));
        vec2 minor = abs(fract(g * 4.0) - 0.5);
        vec2 fwm = fwidth(g * 4.0) * 1.2;
        float minorLines = max(smoothstep(fwm.x, 0.0, 0.5 - minor.x), smoothstep(fwm.y, 0.0, 0.5 - minor.y));

        float r = length(g);
        float fade = exp(-r * 0.16) * smoothstep(60.0, 4.0, dist);
        float ring = 0.0;
        for (int i = 0; i < 3; ++i)
        {
            float rr = fract(t * 0.12 + float(i) / 3.0) * 14.0;
            ring += exp(-abs(r - rr) * 7.0) * (1.0 - rr / 14.0);
        }
        vec3 gridCol = vec3(0.1, 0.65, 1.0);
        col += gridCol * (lines * 0.55 + minorLines * 0.12) * fade;
        col += vec3(0.3, 0.9, 1.0) * ring * fade * 1.6;

        // Contacts drifting on the grid plane.
        for (int i = 0; i < 5; ++i)
        {
            float fi = float(i);
            vec2 pos = vec2(sin(fi * 2.4 + t * 0.07) * (3.0 + fi), cos(fi * 1.7 + t * 0.05) * (2.5 + fi * 0.8) - 2.0);
            float d = length(g - pos);
            float pulse = 0.6 + 0.4 * sin(t * 3.0 + fi);
            vec3 c = (i == 2) ? vec3(1.0, 0.25, 0.15) : vec3(1.0, 0.7, 0.2);
            col += c * (exp(-d * 18.0) * 4.0 + exp(-abs(d - 0.35) * 40.0) * 0.6) * pulse * fade;
        }
    }

    fragColor = vec4(col, 1.0);
}
