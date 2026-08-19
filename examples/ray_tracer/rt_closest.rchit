#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : enable

#include "default_parameters.h"

struct RayPayload
{
    vec3 color;
    float distance;
    vec3 normal;
    float reflector;
};

layout(location = 0) rayPayloadInEXT RayPayload rayPayload;
hitAttributeEXT vec2 attributes;
layout(binding = 2, set = 1, scalar) readonly buffer Vertices
{
    float values[];
} vertices;
layout(binding = 3, set = 1) readonly buffer Indices
{
    uint values[];
} indices;

vec3 vertex3(uint vertexIndex, uint component)
{
    uint stride = ubo.vertexSize / 4;
    uint offset = vertexIndex * stride + component;
    return vec3(vertices.values[offset], vertices.values[offset + 1],
        vertices.values[offset + 2]);
}

void main()
{
    uint i0 = indices.values[gl_PrimitiveID * 3 + 0];
    uint i1 = indices.values[gl_PrimitiveID * 3 + 1];
    uint i2 = indices.values[gl_PrimitiveID * 3 + 2];
    vec3 barycentric = vec3(1.0 - attributes.x - attributes.y,
        attributes.x, attributes.y);
    vec3 color = vertex3(i0, 6) * barycentric.x
        + vertex3(i1, 6) * barycentric.y
        + vertex3(i2, 6) * barycentric.z;
    vec3 normal = normalize(vertex3(i0, 9) * barycentric.x
        + vertex3(i1, 9) * barycentric.y
        + vertex3(i2, 9) * barycentric.z);
    float diffuse = max(dot(abs(normal), normalize(vec3(0.3, 0.6, 0.7))),
        0.0) * 0.75 + 0.25;
    rayPayload.color = color * diffuse;
    rayPayload.distance = gl_HitTEXT;
    rayPayload.normal = normal;
    rayPayload.reflector = 0.0;
}
