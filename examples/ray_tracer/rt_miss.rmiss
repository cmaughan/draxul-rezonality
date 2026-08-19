#version 460
#extension GL_EXT_ray_tracing : require

struct RayPayload
{
    vec3 color;
    float distance;
    vec3 normal;
    float reflector;
};

layout(location = 0) rayPayloadInEXT RayPayload rayPayload;

void main()
{
    rayPayload.color = vec3(0.0);
    rayPayload.distance = -1.0;
    rayPayload.normal = vec3(0.0);
    rayPayload.reflector = 0.0;
}
