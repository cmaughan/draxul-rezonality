#include <catch2/catch_test_macros.hpp>

#include "gpu_resource_transaction.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

// Backend-neutral GPU publication policy and Metal source invariants. These
// cases need neither a native backend nor the plugin ABI.

namespace
{

constexpr std::size_t gpu_stage_index(
    rezonality::detail::GpuInitializationStage stage)
{
    return static_cast<std::size_t>(stage);
}

struct FakeGpuGeneration
{
    uint64_t generation = 0;
    std::array<bool, gpu_stage_index(
                         rezonality::detail::GpuInitializationStage::Count)>
        resources{};
};

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    return { std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>() };
}

std::filesystem::path plugin_root()
{
    return std::filesystem::path(DRAXUL_PROJECT_ROOT)
        / "plugins" / "rezonality";
}

} // namespace

TEST_CASE("Rezonality GPU initialization failures roll back and retry cleanly",
    "[rezonality][gpu][rollback]")
{
    using Stage = rezonality::detail::GpuInitializationStage;
    constexpr std::array stages{
        Stage::VertexBuffer,
        Stage::VertexMemory,
        Stage::VertexMemoryBind,
        Stage::VertexMemoryMap,
        Stage::ModelTextureAttachment,
        Stage::ModelTextureSampler,
        Stage::ModelTextureUpload,
    };
    constexpr std::array failure_stages{
        Stage::VertexMemory,
        Stage::VertexMemoryBind,
        Stage::VertexMemoryMap,
        Stage::ModelTextureSampler,
        Stage::ModelTextureUpload,
    };

    for (const Stage failure_stage : failure_stages)
    {
        CAPTURE(gpu_stage_index(failure_stage));
        int live_resources = static_cast<int>(stages.size());
        FakeGpuGeneration active;
        active.generation = 4;
        active.resources.fill(true);

        const auto destroy = [&](FakeGpuGeneration& generation) {
            for (bool& resource : generation.resources)
            {
                if (resource)
                {
                    resource = false;
                    --live_resources;
                }
            }
        };
        const auto complete = [](const FakeGpuGeneration& generation) {
            return std::ranges::all_of(
                generation.resources, [](bool resource) { return resource; });
        };
        const auto attempt = [&](uint64_t candidate_generation,
                                 std::optional<
                                     rezonality::detail::GpuFailurePoint>
                                     failure_point) {
            return rezonality::detail::publish_gpu_resources_transactionally<
                FakeGpuGeneration>(
                [&](FakeGpuGeneration& candidate) {
                    candidate.generation = candidate_generation;
                    return rezonality::detail::initialize_gpu_resources(
                        stages, failure_point, [&](Stage stage) {
                            candidate.resources[gpu_stage_index(stage)] = true;
                            ++live_resources;
                            return true;
                        });
                },
                complete,
                destroy,
                [&](FakeGpuGeneration&& candidate) {
                    destroy(active);
                    active = std::move(candidate);
                });
        };

        REQUIRE_FALSE(attempt(
            5, rezonality::detail::GpuFailurePoint{ failure_stage }));
        CHECK(active.generation == 4);
        CHECK(complete(active));
        CHECK(live_resources == static_cast<int>(stages.size()));

        REQUIRE(attempt(6, std::nullopt));
        CHECK(active.generation == 6);
        CHECK(complete(active));
        CHECK(live_resources == static_cast<int>(stages.size()));

        destroy(active);
        CHECK(live_resources == 0);
    }
}

TEST_CASE("Rezonality Metal model passes test and write depth",
    "[rezonality][metal][depth]")
{
    const std::string source
        = read_text(plugin_root() / "src" / "native_backend_metal.mm");
    CHECK(source.find("depthCompareFunction = MTLCompareFunctionLessEqual")
        != std::string::npos);
    CHECK(source.find("depthWriteEnabled = YES") != std::string::npos);
    CHECK(source.find("scene_pass.model_index && scene_pass.has_depth")
        != std::string::npos);
    CHECK(source.find("[encoder setDepthStencilState:")
        != std::string::npos);
}
