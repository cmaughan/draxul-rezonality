// Gerstner ocean shared by the ocean meshes and anything that floats on them.
// Optional defines before including:
//   WAVE_AMP   overall amplitude scale (default 1.0)
//   WAVE_WIND  primary travel angle in radians (default 0.0 = toward +Z)
//   WAVE_FLOW  vec2 m/s the wave field drifts past the scene (default 0)
#ifndef SEASIDE_WAVES_GLSL
#define SEASIDE_WAVES_GLSL

#ifndef WAVE_AMP
#define WAVE_AMP 1.0
#endif
#ifndef WAVE_WIND
#define WAVE_WIND 0.0
#endif
#ifndef WAVE_FLOW
#define WAVE_FLOW vec2(0.0)
#endif

#define GERSTNER_WAVES 9

// (wavelength m, amplitude m, steepness 0..1, angle offset from the wind)
const vec4 kWaves[GERSTNER_WAVES] = vec4[](
    vec4(46.0, 0.42, 0.55, 0.0),
    vec4(29.0, 0.27, 0.60, 0.55),
    vec4(21.0, 0.20, 0.62, -0.42),
    vec4(13.0, 0.12, 0.65, 0.95),
    vec4(9.0, 0.075, 0.60, -0.85),
    vec4(6.1, 0.05, 0.55, 0.25),
    vec4(4.3, 0.032, 0.50, -1.3),
    vec4(3.1, 0.022, 0.45, 1.6),
    vec4(2.2, 0.014, 0.40, -0.2)
);

vec2 waveSample(vec2 xz, float t)
{
    return xz + WAVE_FLOW * t;
}

// Displacement of the undisplaced surface point xz. lod in [0,1] fades the
// short waves for distant, sparsely tessellated vertices.
vec3 gerstnerDisplace(vec2 xz, float t, float lod)
{
    vec2 p = waveSample(xz, t);
    vec3 d = vec3(0.0);
    for (int i = 0; i < GERSTNER_WAVES; ++i)
    {
        vec4 w = kWaves[i];
        float fade = 1.0 - lod * smoothstep(20.0, 3.0, w.x);
        float base = w.y * WAVE_AMP;
        float a = base * fade;
        float k = TAU / w.x;
        float c = sqrt(9.81 / k);
        float ang = WAVE_WIND + w.w;
        vec2 dir = vec2(sin(ang), cos(ang));
        float ph = k * dot(dir, p) - k * c * t + float(i) * 1.7;
        float q = w.z / (k * base * float(GERSTNER_WAVES) + 1e-4);
        d.xz += q * a * dir * cos(ph);
        d.y += a * sin(ph);
    }
    return d;
}

// Surface normal from the same wave train plus extra short ripples, and a
// crest factor (Jacobian compression) for foam in .w.
vec4 gerstnerNormal(vec2 xz, float t, float detail)
{
    vec2 p = waveSample(xz, t);
    vec3 n = vec3(0.0, 1.0, 0.0);
    float jac = 0.0;
    for (int i = 0; i < GERSTNER_WAVES; ++i)
    {
        vec4 w = kWaves[i];
        float a = w.y * WAVE_AMP;
        float k = TAU / w.x;
        float c = sqrt(9.81 / k);
        float ang = WAVE_WIND + w.w;
        vec2 dir = vec2(sin(ang), cos(ang));
        float ph = k * dot(dir, p) - k * c * t + float(i) * 1.7;
        float q = w.z / (k * a * float(GERSTNER_WAVES) + 1e-4);
        float wa = k * a;
        float s = sin(ph), co = cos(ph);
        n.x -= dir.x * wa * co;
        n.z -= dir.y * wa * co;
        n.y -= q * wa * s;
        jac += q * wa * s;
    }
    // Capillary ripples: cheap directional sines, faded by the caller.
    float amp = 0.035 * detail * WAVE_AMP;
    float freq = 2.3;
    float ang = WAVE_WIND + 0.3;
    for (int i = 0; i < 6; ++i)
    {
        vec2 dir = vec2(sin(ang), cos(ang));
        float ph = dot(dir, p) * freq - t * sqrt(9.81 * freq) + float(i) * 2.1;
        float slope = amp * freq * cos(ph);
        n.xz -= dir * slope;
        amp *= 0.62;
        freq *= 1.6;
        ang += 2.4;
    }
    return vec4(normalize(n), jac);
}

#endif
