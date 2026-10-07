// Moon, sky and lantern lighting for the night models.
#ifndef SEASIDE_NIGHT_LIGHTING_GLSL
#define SEASIDE_NIGHT_LIGHTING_GLSL

vec3 nightLight(vec3 p, vec3 albedo, vec3 n, vec3 v, float gloss, float spec)
{
    float ndl = max(dot(n, MOON_DIR), 0.0);
    vec3 col = albedo * (MOON_COL * 0.32 * ndl + vec3(0.03, 0.04, 0.075) * (0.6 + 0.4 * n.y));
    vec3 h = normalize(MOON_DIR + v);
    col += MOON_COL * spec * pow(max(dot(n, h), 0.0), gloss) * 0.4;
    // Silver rim where the moon grazes silhouettes.
    float rim = pow(1.0 - max(dot(n, v), 0.0), 4.0) * smoothstep(-0.3, 0.6, dot(-v, MOON_DIR));
    col += MOON_COL * rim * 0.035 * (0.3 + 0.7 * max(dot(n, MOON_DIR), 0.0));
    // Warm spill from the lantern room.
    vec3 toLamp = LAMP_POS - p;
    float d2 = dot(toLamp, toLamp);
    float lampN = max(dot(n, normalize(toLamp)), 0.0);
    col += albedo * LAMP_COL * lampN * 6.0 / (1.0 + d2 * 0.25);
    // Cool glow thrown up from the bioluminescent surf.
    float surf = smoothstep(2.5, -0.5, p.y) * smoothstep(-6.0, 1.0, -abs(shoreDistance(p.xz)));
    col += albedo * BIO_COL * surf * 0.5 * (0.5 + 0.5 * max(-n.y + 0.5, 0.0));
    return col;
}

#endif
