#pragma once

#include <cstddef>
#include <optional>
#include <utility>

namespace rezonality::detail
{

enum class GpuInitializationStage
{
    VertexBuffer,
    VertexMemory,
    VertexMemoryBind,
    VertexMemoryMap,
    ModelTextureAttachment,
    ModelTextureSampler,
    ModelTextureUpload,
    Count,
};

struct GpuFailurePoint
{
    GpuInitializationStage stage = GpuInitializationStage::VertexMemory;
};

// Apply the deterministic failure after the selected callback. This models an
// API that has populated a handle before reporting failure and makes cleanup of
// partially initialized resources observable in platform-neutral tests.
template <typename Stages, typename CreateStage>
bool initialize_gpu_resources(
    const Stages& stages,
    const std::optional<GpuFailurePoint>& failure_point,
    CreateStage&& create_stage)
{
    for (const GpuInitializationStage stage : stages)
    {
        if (!create_stage(stage))
            return false;
        if (failure_point.has_value() && failure_point->stage == stage)
            return false;
    }
    return true;
}

template <typename Candidate, typename Build, typename Complete, typename Destroy, typename Publish>
bool publish_gpu_resources_transactionally(
    Build&& build,
    Complete&& complete,
    Destroy&& destroy,
    Publish&& publish)
{
    Candidate candidate{};
    if (!build(candidate) || !complete(candidate))
    {
        destroy(candidate);
        return false;
    }
    publish(std::move(candidate));
    return true;
}

} // namespace rezonality::detail
