#include <catch2/catch_test_macros.hpp>

#import <Metal/Metal.h>

#include "camera.h"
#include "live_project.h"
#include "native_backend.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

namespace fs = std::filesystem;

fs::path plugin_root()
{
    return fs::path(DRAXUL_PROJECT_ROOT) / "plugins" / "rezonality";
}

void write_text(const fs::path& path, std::string_view contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output << contents;
    REQUIRE(output.good());
}

struct ColorTargetProject
{
    explicit ColorTargetProject(size_t color_targets)
        : path(fs::temp_directory_path()
              / ("draxul-rezonality-color-targets-"
                  + std::to_string(color_targets) + "-"
                  + std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count())))
    {
        REQUIRE(fs::create_directories(path));
        std::error_code ec;
        fs::copy_file(plugin_root() / "examples" / "simple" / "screen.vert",
            path / "screen.vert", ec);
        REQUIRE_FALSE(ec);
        std::string scenegraph;
        std::string targets;
        std::string fragment = "#version 450\n";
        for (size_t index = 0; index < color_targets; ++index)
        {
            const std::string name = "Target" + std::to_string(index);
            scenegraph += "surface: " + name
                + " { scale: (1, 1, 1) format: default_color }\n";
            targets += (index == 0 ? "" : ", ") + name;
        }
        // The shader stays within Metal's output range so the target list,
        // not shader translation, is what exceeds the attachment slots.
        const size_t outputs = std::min<size_t>(color_targets, 8);
        for (size_t index = 0; index < outputs; ++index)
            fragment += "layout(location = " + std::to_string(index)
                + ") out vec4 out" + std::to_string(index) + ";\n";
        scenegraph += "pass: Outputs {\n    targets: (" + targets
            + ")\n    clear: (0.0, 0.0, 0.0, 1.0)\n"
              "    geometry: background { path: screen_rect vs: screen.vert "
              "fs: outputs.frag }\n}\n";
        fragment += "void main()\n{\n";
        for (size_t index = 0; index < outputs; ++index)
            fragment += "    out" + std::to_string(index)
                + " = vec4(1.0, 0.0, 0.0, 1.0);\n";
        fragment += "}\n";
        write_text(path / "default.scenegraph", scenegraph);
        write_text(path / "outputs.frag", fragment);
    }

    ~ColorTargetProject()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path path;
};

rezonality::BackendPreparation prepare_color_targets(size_t color_targets)
{
    ColorTargetProject project(color_targets);
    rezonality::ProjectOptions options;
    options.project_path = project.path;
    options.scenegraph = "default.scenegraph";
    const rezonality::ProjectPipeline pipeline(plugin_root(), options);
    const auto built = pipeline.build(1);
    INFO(built.error);
    REQUIRE(built.build);
    REQUIRE(built.build->passes.size() == 1);
    REQUIRE(built.build->passes.front().targets.size() == color_targets);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:64
                                    height:64
                                 mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> drawable = [device newTextureWithDescriptor:descriptor];
    REQUIRE(drawable != nil);

    DraxulPluginMetalFrameV2 frame{};
    frame.struct_size = sizeof(frame);
    frame.device = (__bridge void*)device;
    frame.drawable_texture = (__bridge void*)drawable;
    frame.buffered_frame_count = 2;
    frame.framebuffer_width = 64;
    frame.framebuffer_height = 64;
    frame.viewport = { sizeof(DraxulPluginViewportV2), 0, 0, 64, 64, 1.0f,
        96.0f };
    rezonality::Camera camera;
    rezonality::NativeBackend backend;
    backend.bind_frame(frame, 0.0, camera);
    return backend.prepare(*built.build);
}

} // namespace

TEST_CASE("Rezonality Metal accepts the color-target limit and rejects more",
    "[rezonality][metal][color-targets]")
{
    const auto at_limit = prepare_color_targets(8);
    CHECK(at_limit.ready);
    CHECK(at_limit.error.empty());

    const auto over_limit = prepare_color_targets(9);
    INFO(over_limit.error);
    CHECK_FALSE(over_limit.ready);
    CHECK(over_limit.error.find("9 color targets") != std::string::npos);
    CHECK(over_limit.error.find("at most 8") != std::string::npos);
}

namespace
{

// A borrowed Draxul Metal frame with its own drawable and continuation pass.
struct MetalTestFrame
{
    MetalTestFrame(id<MTLDevice> device, int width, int height,
        MTLPixelFormat format)
    {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:format
                                         width:static_cast<NSUInteger>(width)
                                        height:static_cast<NSUInteger>(height)
                                     mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget;
        descriptor.storageMode = MTLStorageModePrivate;
        drawable = [device newTextureWithDescriptor:descriptor];
        REQUIRE(drawable != nil);
        continuation = [MTLRenderPassDescriptor renderPassDescriptor];
        continuation.colorAttachments[0].texture = drawable;
        continuation.colorAttachments[0].loadAction = MTLLoadActionClear;
        continuation.colorAttachments[0].storeAction = MTLStoreActionStore;
        frame.struct_size = sizeof(frame);
        frame.device = (__bridge void*)device;
        frame.drawable_texture = (__bridge void*)drawable;
        frame.continuation_render_pass_descriptor
            = (__bridge void*)continuation;
        frame.buffered_frame_count = 2;
        frame.framebuffer_width = width;
        frame.framebuffer_height = height;
        frame.viewport = { sizeof(DraxulPluginViewportV2), 0, 0, width,
            height, 1.0f, 96.0f };
    }

    id<MTLTexture> drawable = nil;
    MTLRenderPassDescriptor* continuation = nil;
    DraxulPluginMetalFrameV2 frame{};
};

void record_and_wait(rezonality::NativeBackend& backend,
    id<MTLCommandQueue> queue, MetalTestFrame& test_frame,
    uint32_t frame_index)
{
    id<MTLCommandBuffer> command = [queue commandBuffer];
    REQUIRE(command != nil);
    test_frame.frame.command_buffer = (__bridge void*)command;
    test_frame.frame.frame_index = frame_index;
    const auto result = backend.record(test_frame.frame, nullptr, true);
    test_frame.frame.command_buffer = nullptr;
    CHECK(result.ok);
    [command commit];
    [command waitUntilCompleted];
    INFO((command.error ? command.error.localizedDescription.UTF8String
                        : "no command-buffer error"));
    CHECK(command.status == MTLCommandBufferStatusCompleted);
}

} // namespace

TEST_CASE("Rezonality Metal resize keeps models, images, and pipelines",
    "[rezonality][metal][resize]")
{
    rezonality::ProjectOptions options;
    options.project_path = plugin_root() / "examples" / "pbr_robot";
    options.scenegraph = "default.scenegraph";
    const rezonality::ProjectPipeline pipeline(plugin_root(), options);
    const auto built = pipeline.build(1);
    INFO(built.error);
    REQUIRE(built.build);
    const auto& build = *built.build;
    REQUIRE(build.models.size() == 1);
    size_t static_images = 0;
    for (size_t index = 0; index < build.surfaces.size(); ++index)
        if (rezonality::surface_is_viewport_independent(build, index))
            ++static_images;
    REQUIRE(static_images == 1);
    const size_t viewport_surfaces = build.surfaces.size() - static_images;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    REQUIRE(queue != nil);
    rezonality::Camera camera;
    rezonality::NativeBackend backend;

    MetalTestFrame initial(device, 64, 64, MTLPixelFormatBGRA8Unorm);
    backend.bind_frame(initial.frame, 0.0, camera);
    REQUIRE(backend.prepare(build).ready);
    backend.activate_prepared();
    const auto first = backend.resource_stats();
    CHECK(first.last_reuse == rezonality::GenerationReuse::Rebuild);
    CHECK(first.generations_prepared == 1);
    CHECK(first.models_created == 1);
    CHECK(first.programs_created == build.passes.size());
    CHECK(first.surfaces_created == build.surfaces.size());
    CHECK(first.asset_upload_bytes > 0);
    CHECK(backend.active_compatible());
    record_and_wait(backend, queue, initial, 0);
    // Metal uploads static pixels with replaceRegion and keeps no
    // plugin-owned staging buffers.
    CHECK(backend.resource_stats().retained_upload_staging_bytes == 0);

    // Size-only change: only viewport-sized surfaces are recreated.
    MetalTestFrame resized(device, 96, 80, MTLPixelFormatBGRA8Unorm);
    backend.bind_frame(resized.frame, 0.0, camera);
    CHECK_FALSE(backend.active_compatible());
    const auto resize_preparation = backend.prepare(build);
    INFO(resize_preparation.error);
    REQUIRE(resize_preparation.ready);
    backend.activate_prepared();
    const auto second = backend.resource_stats();
    CHECK(second.last_reuse == rezonality::GenerationReuse::ResizeTargets);
    CHECK(second.generations_prepared == 2);
    CHECK(second.models_created == first.models_created);
    CHECK(second.models_reused == 1);
    CHECK(second.programs_created == first.programs_created);
    CHECK(second.programs_reused == build.passes.size());
    CHECK(second.surfaces_created
        == first.surfaces_created + viewport_surfaces);
    CHECK(second.surfaces_reused == static_images);
    CHECK(second.asset_upload_bytes == first.asset_upload_bytes);
    CHECK(backend.active_compatible());
    record_and_wait(backend, queue, resized, 1);
    backend.retire_completed_slot(0);
    backend.retire_completed_slot(1);

    // A size beyond the device limit is rejected without losing the active
    // generation or its shared assets.
    MetalTestFrame oversized(device, 64, 64, MTLPixelFormatBGRA8Unorm);
    oversized.frame.viewport.width = 40000;
    backend.bind_frame(oversized.frame, 0.0, camera);
    const auto rejected = backend.prepare(build);
    CHECK_FALSE(rejected.ready);
    CHECK(rejected.error.find("exceeds the device 2D texture limit")
        != std::string::npos);
    backend.bind_frame(resized.frame, 0.0, camera);
    CHECK(backend.active_compatible());
    record_and_wait(backend, queue, resized, 0);

    // A new drawable format keeps source assets but rebuilds pipelines.
    MetalTestFrame reformatted(device, 96, 80, MTLPixelFormatRGBA16Float);
    backend.bind_frame(reformatted.frame, 0.0, camera);
    CHECK_FALSE(backend.active_compatible());
    REQUIRE(backend.prepare(build).ready);
    backend.activate_prepared();
    const auto third = backend.resource_stats();
    CHECK(third.last_reuse == rezonality::GenerationReuse::ReuseAssets);
    CHECK(third.models_created == first.models_created);
    CHECK(third.programs_created
        == first.programs_created + build.passes.size());
    CHECK(third.asset_upload_bytes == first.asset_upload_bytes);
    record_and_wait(backend, queue, reformatted, 1);

    // A different source build never shares native resources.
    auto rebuilt = build;
    rebuilt.generation = build.generation + 1;
    REQUIRE(backend.prepare(rebuilt).ready);
    backend.activate_prepared();
    const auto fourth = backend.resource_stats();
    CHECK(fourth.last_reuse == rezonality::GenerationReuse::Rebuild);
    CHECK(fourth.models_created == first.models_created + 1);
    CHECK(fourth.asset_upload_bytes == 2 * first.asset_upload_bytes);
}
