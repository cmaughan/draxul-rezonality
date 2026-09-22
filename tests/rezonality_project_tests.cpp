#include <catch2/catch_test_macros.hpp>

#include "live_project.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

namespace
{

namespace fs = std::filesystem;

fs::path plugin_root()
{
    return fs::path(DRAXUL_REZONALITY_TEST_ROOT);
}

struct ProjectFixture
{
    explicit ProjectFixture(std::string_view example)
        : path(fs::temp_directory_path()
              / ("draxul-rezonality-project-" + std::string(example)))
    {
        std::error_code error;
        fs::remove_all(path, error);
        error.clear();
        fs::copy(plugin_root() / "examples" / example, path,
            fs::copy_options::recursive, error);
        REQUIRE_FALSE(error);
    }

    ~ProjectFixture()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
};

struct RestoreRenamedDirectory
{
    ~RestoreRenamedDirectory()
    {
        std::error_code error;
        if (!fs::exists(original, error) && fs::exists(renamed, error))
            fs::rename(renamed, original, error);
    }

    fs::path original;
    fs::path renamed;
};

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    return { std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>() };
}

void write(const fs::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output << text;
    REQUIRE(output.good());
}

bool accept_shader(const fs::path&, std::vector<uint32_t>& spirv,
    std::vector<rezonality::DiagnosticEntry>&)
{
    spirv = { 0x07230203u };
    return true;
}

std::optional<rezonality::BuildResult> wait_for_result(
    rezonality::LiveProject& project)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto result = project.take_result())
            return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
}

rezonality::ProjectOptions options(const fs::path& path)
{
    rezonality::ProjectOptions result;
    result.project_path = path;
    return result;
}

} // namespace

TEST_CASE("Rezonality project pipeline contains malformed numeric input",
    "[rezonality][project]")
{
    ProjectFixture fixture("simple");
    const fs::path scenegraph = fixture.path / "default.scenegraph";
    const std::string original = read(scenegraph);
    write(scenegraph, original
        + "\ncamera: Incomplete { field_of_view: - }\n");

    rezonality::ProjectPipeline pipeline(
        plugin_root(), options(fixture.path), accept_shader);
    const auto failed = pipeline.build(7);
    CHECK_FALSE(failed.build);
    CHECK(failed.generation == 7);
    CHECK(failed.diagnostic_path.filename() == "default.scenegraph");
    CHECK(failed.error.find("invalid finite number") != std::string::npos);

    write(scenegraph, original
        + "\nsurface: Incomplete { scale: (1, - }\n");
    const auto incomplete_vector = pipeline.build(8);
    CHECK_FALSE(incomplete_vector.build);
    CHECK(incomplete_vector.error.find("invalid numeric value")
        != std::string::npos);

    write(scenegraph, original
        + "\ncamera: Overflow { field_of_view: 1e999 }\n");
    const auto overflow = pipeline.build(9);
    CHECK_FALSE(overflow.build);
    CHECK(overflow.error.find("invalid finite number")
        != std::string::npos);

    write(scenegraph, original);
    const auto repaired = pipeline.build(10);
    REQUIRE(repaired.build);
    CHECK(repaired.generation == 10);
}

TEST_CASE("Rezonality project pipeline rejects image format mismatches",
    "[rezonality][project]")
{
    ProjectFixture fixture("default");
    const fs::path scenegraph = fixture.path / "default.scenegraph";
    std::ifstream input(scenegraph, std::ios::binary);
    REQUIRE(input);
    std::string scene{ std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>() };
    const std::string original = "surface: Noise { path: noise.png }";
    const auto position = scene.find(original);
    REQUIRE(position != std::string::npos);
    scene.replace(position, original.size(),
        "surface: Noise { path: noise.png format: rgba16f }");
    std::ofstream output(scenegraph, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output << scene;
    output.close();

    rezonality::ProjectPipeline pipeline(
        plugin_root(), options(fixture.path), accept_shader);
    const auto failed = pipeline.build(3);
    CHECK_FALSE(failed.build);
    CHECK(failed.error.find("incompatible with decoded pixel storage")
        != std::string::npos);

    const std::string floating
        = "surface: Noise { path: noise.png format: rgba16f }";
    auto updated = scene.find(floating);
    REQUIRE(updated != std::string::npos);
    scene.replace(updated, floating.size(),
        "surface: Noise { path: noise.png format: default_depth }");
    write(scenegraph, scene);
    CHECK_FALSE(pipeline.build(4).build);

    const std::string depth
        = "surface: Noise { path: noise.png format: default_depth }";
    updated = scene.find(depth);
    REQUIRE(updated != std::string::npos);
    scene.replace(updated, depth.size(),
        "surface: Noise { path: noise.png format: rgba8 }");
    write(scenegraph, scene);
    const auto compatible = pipeline.build(5);
    REQUIRE(compatible.build);
    const auto noise = std::ranges::find(
        compatible.build->surfaces, "Noise",
        &rezonality::ShaderBuild::Surface::name);
    REQUIRE(noise != compatible.build->surfaces.end());
    CHECK(noise->format == rezonality::ShaderBuild::SurfaceFormat::Color8);
    CHECK(noise->image_pixels.size()
        == static_cast<size_t>(noise->image_width) * noise->image_height * 4);
}

TEST_CASE("Rezonality project pipeline validates HDR format overrides",
    "[rezonality][project]")
{
    ProjectFixture fixture("simple");
    const fs::path hdr = fixture.path / "sky.hdr";
    std::error_code error;
    fs::copy_file(plugin_root() / "examples" / "pbr_robot" / "textures"
            / "environment" / "farm_field_puresky_1k.hdr",
        hdr, fs::copy_options::overwrite_existing, error);
    REQUIRE_FALSE(error);

    const fs::path scenegraph = fixture.path / "default.scenegraph";
    const std::string original = read(scenegraph);
    rezonality::ProjectPipeline pipeline(
        plugin_root(), options(fixture.path), accept_shader);

    write(scenegraph, original
        + "\nenvironment: Sky { path: sky.hdr format: rgba16f }\n");
    const auto incompatible = pipeline.build(4);
    CHECK_FALSE(incompatible.build);
    CHECK(incompatible.error.find("incompatible with decoded pixel storage")
        != std::string::npos);

    write(scenegraph, original
        + "\nenvironment: Sky { path: sky.hdr format: default_depth }\n");
    CHECK_FALSE(pipeline.build(5).build);

    write(scenegraph, original
        + "\nenvironment: Sky { path: sky.hdr format: rgba32f }\n");
    const auto compatible = pipeline.build(6);
    REQUIRE(compatible.build);
    const auto sky = std::ranges::find(
        compatible.build->surfaces, "Sky",
        &rezonality::ShaderBuild::Surface::name);
    REQUIRE(sky != compatible.build->surfaces.end());
    CHECK(sky->format
        == rezonality::ShaderBuild::SurfaceFormat::Color32Float);
    CHECK(sky->image_float_pixels.size()
        == static_cast<size_t>(sky->image_width) * sky->image_height * 4);
}

TEST_CASE("Rezonality validates decoded image upload storage",
    "[rezonality][project][upload]")
{
    using Format = rezonality::ShaderBuild::SurfaceFormat;
    rezonality::ShaderBuild::Surface surface;
    surface.name = "Image";
    surface.image_width = 2;
    surface.image_height = 3;
    surface.format = Format::Color8;
    surface.image_pixels.resize(24);
    std::string error;
    CHECK(rezonality::validate_surface_upload_storage(surface, error));

    surface.format = Format::Color16Float;
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));
    surface.format = Format::Depth32;
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));

    surface.format = Format::Color8;
    surface.image_pixels.resize(23);
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));

    surface.image_pixels.resize(24);
    surface.image_float_pixels.resize(24);
    surface.format = Format::Color32Float;
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));
    surface.image_pixels.clear();
    CHECK(rezonality::validate_surface_upload_storage(surface, error));
    surface.format = Format::Color8;
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));

    surface.image_float_pixels.resize(23);
    surface.format = Format::Color32Float;
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));
    CHECK(error.find("invalid upload storage") != std::string::npos);
}

TEST_CASE("Rezonality live watch reports filesystem failure and recovers",
    "[rezonality][project][watch]")
{
    ProjectFixture fixture("simple");
    const fs::path unavailable = fixture.path.string() + ".unavailable";
    std::error_code error;
    fs::remove_all(unavailable, error);
    RestoreRenamedDirectory restore{ fixture.path, unavailable };
    auto configured = options(fixture.path);
    configured.compile_debounce_ms = 0;
    rezonality::LiveProject live(plugin_root(), configured, [] {},
        accept_shader);
    live.start();

    auto initial = wait_for_result(live);
    REQUIRE(initial);
    REQUIRE(initial->build);

    fs::rename(fixture.path, unavailable, error);
    REQUIRE_FALSE(error);
    auto failed = wait_for_result(live);
    REQUIRE(failed);
    CHECK_FALSE(failed->build);
    CHECK(failed->error.find("project watch failed") != std::string::npos);
    CHECK(failed->generation > initial->generation);

    fs::rename(unavailable, fixture.path, error);
    REQUIRE_FALSE(error);
    auto recovered = wait_for_result(live);
    REQUIRE(recovered);
    REQUIRE(recovered->build);
    CHECK(recovered->generation > failed->generation);
    live.stop();
}

TEST_CASE("Rezonality project pipeline reports injected compiler failures",
    "[rezonality][project]")
{
    ProjectFixture fixture("simple");
    auto configured = options(fixture.path);

    rezonality::ProjectPipeline empty_compiler(plugin_root(), configured,
        [](const fs::path&, std::vector<uint32_t>&,
            std::vector<rezonality::DiagnosticEntry>&) {
            return true;
        });
    const auto empty = empty_compiler.build(9);
    CHECK_FALSE(empty.build);
    CHECK(empty.error.find("produced no SPIR-V") != std::string::npos);

    rezonality::ProjectPipeline failed_compiler(plugin_root(), configured,
        [](const fs::path& shader, std::vector<uint32_t>&,
            std::vector<rezonality::DiagnosticEntry>& diagnostics) {
            diagnostics.push_back({
                .path = shader,
                .stage = "compile",
                .severity = "error",
                .line = 12,
                .message = "injected compiler diagnostic",
            });
            return false;
        });
    const auto failed = failed_compiler.build(10);
    CHECK_FALSE(failed.build);
    CHECK(failed.diagnostic_line == 12);
    CHECK(failed.error == "injected compiler diagnostic");
}
