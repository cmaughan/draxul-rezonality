// Boat placement on the sunset swell, shared by the hull and the wake.
#ifndef SEASIDE_BOAT_MOTION_GLSL
#define SEASIDE_BOAT_MOTION_GLSL

// Heading in the XZ plane (model forward is -Z) and its starboard side.
const vec2 BOAT_FWD = normalize(vec2(-1.0, 0.22));
const vec2 BOAT_RIGHT = vec2(-BOAT_FWD.y, BOAT_FWD.x);

// Surface height under a world xz point (one fixed-point step undoes the
// Gerstner horizontal shift). Short waves are ignored: the hull averages them.
float seaHeightAt(vec2 xz, float t)
{
    vec3 d = gerstnerDisplace(xz, t, 1.0);
    d = gerstnerDisplace(xz - d.xz, t, 1.0);
    return d.y;
}

#endif
