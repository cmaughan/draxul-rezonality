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
