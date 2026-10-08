#pragma once

#include "live_project.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace rezonality
{

// How much of the active native generation a newly requested generation may
// keep. Native backends classify each preparation with the same rules so a
// pane resize keeps immutable scene assets on both Vulkan and Metal.
enum class GenerationReuse
{
    // No active generation, a different device, or a different source build:
    // every native resource is created again.
    Rebuild,
    // Same source build and device, but the presentation target changed
    // (Vulkan continuation render pass or target generation, Metal pixel
    // format): keep models and static images, rebuild programs and
    // viewport-sized targets.
    ReuseAssets,
    // Same source build, device, and presentation target; only the viewport
    // size changed: keep models, static images, and programs, and rebuild
    // only viewport-sized targets and their bindings.
    ResizeTargets,
    // Nothing changed.
    Compatible,
};

// Identity of the inputs a native generation was prepared from. Handles are
// stored as integers so the rule stays independent of graphics headers.
struct GenerationShape
{
    uint64_t source_generation = 0;
    uint64_t device = 0;
    uint64_t presentation = 0;
    uint64_t presentation_generation = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

[[nodiscard]] inline GenerationReuse classify_generation_reuse(
    const GenerationShape* active, const GenerationShape& requested)
{
    if (!active || active->device != requested.device
        || active->source_generation != requested.source_generation)
        return GenerationReuse::Rebuild;
    if (active->presentation != requested.presentation
        || active->presentation_generation
            != requested.presentation_generation)
        return GenerationReuse::ReuseAssets;
    if (active->width != requested.width
        || active->height != requested.height)
        return GenerationReuse::ResizeTargets;
    return GenerationReuse::Compatible;
}

// True when the surface's native image may be shared between generations of
// the same source build regardless of pane size: it has fixed image
// dimensions and static uploaded pixels, is not refreshed from audio, and no
// pass renders into it (so its contents never diverge from the source image).
[[nodiscard]] inline bool surface_is_viewport_independent(
    const ShaderBuild& build, size_t surface_index)
{
    if (surface_index >= build.surfaces.size())
        return false;
    const auto& surface = build.surfaces[surface_index];
    if (surface.audio_analysis || surface.image_width == 0
        || surface.image_height == 0
        || (surface.image_pixels.empty() && surface.image_float_pixels.empty()))
        return false;
    return std::none_of(build.passes.begin(), build.passes.end(),
        [&surface](const ShaderBuild::Pass& pass) {
            return std::find(pass.targets.begin(), pass.targets.end(),
                       surface.name)
                != pass.targets.end();
        });
}

// CPU bytes a backend copies to the GPU when it creates the surface's static
// image (zero for render targets and audio surfaces).
[[nodiscard]] inline uint64_t surface_upload_bytes(
    const ShaderBuild::Surface& surface)
{
    if (surface.audio_analysis)
        return 0;
    return !surface.image_float_pixels.empty()
        ? surface.image_float_pixels.size() * sizeof(float)
        : surface.image_pixels.size();
}

// CPU bytes a backend copies to the GPU when it creates the model's vertex,
// index, and material texture resources.
[[nodiscard]] inline uint64_t model_upload_bytes(const ModelData& model)
{
    uint64_t bytes = model.vertices.size() * sizeof(ModelVertex)
        + model.indices.size() * sizeof(uint32_t);
    for (const auto& material : model.materials)
        for (const ModelTexture* texture : { &material.base_color,
                 &material.normal, &material.metallic_roughness,
                 &material.emissive, &material.occlusion })
            bytes += texture->pixels.size();
    return bytes;
}

// Cumulative native work done by one backend instance's preparations. Tests
// and resize measurements use it to prove that a size-only change does not
// recreate source assets or programs.
struct BackendResourceStats
{
    uint64_t generations_prepared = 0;
    uint64_t surfaces_created = 0;
    uint64_t surfaces_reused = 0;
    uint64_t models_created = 0;
    uint64_t models_reused = 0;
    // Raster/ray pipelines (Vulkan) or pipeline states (Metal) compiled.
    uint64_t programs_created = 0;
    uint64_t programs_reused = 0;
    // Static image and model bytes copied for newly created resources.
    uint64_t asset_upload_bytes = 0;
    GenerationReuse last_reuse = GenerationReuse::Rebuild;
};

} // namespace rezonality
