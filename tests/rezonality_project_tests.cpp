#include <catch2/catch_test_macros.hpp>

#include "live_project.h"

#include <filesystem>
#include <fstream>
#include <string>

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

void append(const fs::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::app);
    REQUIRE(output);
    output << text;
    REQUIRE(output.good());
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
    append(fixture.path / "default.scenegraph",
        "\ncamera: Incomplete { field_of_view: - }\n");

    rezonality::ProjectPipeline pipeline(plugin_root(), options(fixture.path));
    const auto failed = pipeline.build(7);
    CHECK_FALSE(failed.build);
    CHECK(failed.generation == 7);
    CHECK(failed.diagnostic_path.filename() == "default.scenegraph");
    CHECK(failed.error.find("invalid finite number") != std::string::npos);

    ProjectFixture overflow("simple");
    append(overflow.path / "default.scenegraph",
        "\ncamera: Overflow { field_of_view: 999999999999999999999999999999999999999999999 }\n");
    rezonality::ProjectPipeline overflow_pipeline(
        plugin_root(), options(overflow.path));
    CHECK_FALSE(overflow_pipeline.build(8).build);
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

    rezonality::ProjectPipeline pipeline(plugin_root(), options(fixture.path));
    const auto failed = pipeline.build(3);
    CHECK_FALSE(failed.build);
    CHECK(failed.error.find("incompatible with decoded pixel storage")
        != std::string::npos);
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
