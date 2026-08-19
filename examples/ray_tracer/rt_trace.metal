#include <metal_stdlib>
#include <metal_raytracing>

using namespace metal;
using namespace metal::raytracing;

struct VklPassChannel { float4 resolution; float time; };
struct VklPassUBO
{
    float iTime;
    float iGlobalTime;
    float iTimeDelta;
    float iFrame;
    float iFrameRate;
    float iSampleRate;
    uint iSceneFlags;
    uint vertexSize;
    float4 iResolution;
    float4 iMouse;
    float4 iDate;
    float4 iSpectrumBands[2];
    float4 iChannelTime;
    float4 iChannelResolution[4];
    float4 ifFragCoordOffsetUniform;
    float4 eye;
    float4x4 model;
    float4x4 view;
    float4x4 projection;
    float4x4 modelViewProjection;
    float4x4 viewInverse;
    float4x4 projectionInverse;
    VklPassChannel iChannel[4];
};

static float3 vertex3(const device float* vertices, uint vertexIndex,
    uint strideFloats, uint component)
{
    const device float* value
        = vertices + vertexIndex * strideFloats + component;
    return float3(value[0], value[1], value[2]);
}

kernel void vklive_ray_trace(
    texture2d<float, access::write> outImage [[texture(0)]],
    instance_acceleration_structure sceneAS [[buffer(0)]],
    constant VklPassUBO& ubo [[buffer(1)]],
    const device float* vertices [[buffer(2)]],
    const device uint* indices [[buffer(3)]],
    uint2 tid [[thread_position_in_grid]])
{
    uint width = outImage.get_width();
    uint height = outImage.get_height();
    if (tid.x >= width || tid.y >= height)
        return;

    float2 uv = (float2(tid) + 0.5) / float2(width, height);
    float2 ndc = uv * 2.0 - 1.0;
    float4 viewTarget = ubo.projectionInverse * float4(ndc, 1.0, 1.0);
    float3 viewDirection = normalize(viewTarget.xyz / viewTarget.w);
    float3 worldDirection
        = normalize((ubo.viewInverse * float4(viewDirection, 0.0)).xyz);

    ray ray;
    ray.origin = ubo.eye.xyz;
    ray.direction = worldDirection;
    ray.min_distance = 0.001;
    ray.max_distance = 10000.0;
    intersector<triangle_data, instancing> tracer;
    intersection_result<triangle_data, instancing> hit
        = tracer.intersect(ray, sceneAS, 0xff);
    if (hit.type == intersection_type::none)
    {
        outImage.write(float4(0.0, 0.0, 0.0, 1.0), tid);
        return;
    }

    uint i0 = indices[hit.primitive_id * 3 + 0];
    uint i1 = indices[hit.primitive_id * 3 + 1];
    uint i2 = indices[hit.primitive_id * 3 + 2];
    uint stride = ubo.vertexSize / 4;
    float2 bary = hit.triangle_barycentric_coord;
    float3 weights = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 color = vertex3(vertices, i0, stride, 6) * weights.x
        + vertex3(vertices, i1, stride, 6) * weights.y
        + vertex3(vertices, i2, stride, 6) * weights.z;
    float3 normal = normalize(vertex3(vertices, i0, stride, 9) * weights.x
        + vertex3(vertices, i1, stride, 9) * weights.y
        + vertex3(vertices, i2, stride, 9) * weights.z);
    float diffuse = saturate(dot(abs(normal),
        normalize(float3(0.3, 0.6, 0.7)))) * 0.75 + 0.25;
    outImage.write(float4(color * diffuse, 1.0), tid);
}
