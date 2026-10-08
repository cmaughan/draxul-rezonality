#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "camera.h"
#include "diagnostics.h"
#include "image_loader.h"
#include "model_loader.h"

#include <nlohmann/json.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Diagnostics publication, model/image loading, camera, and example-content
// cases. They exercise only the CPU project library.

namespace
{

void write_text(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output << contents;
    REQUIRE(output.good());
}

std::filesystem::path plugin_root()
{
    return std::filesystem::path(DRAXUL_PROJECT_ROOT)
        / "plugins" / "rezonality";
}

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    return { std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>() };
}

nlohmann::json read_json(const std::filesystem::path& path)
{
    return nlohmann::json::parse(read_text(path));
}

} // namespace

TEST_CASE("Rezonality diagnostics publish bounded valid UTF-8",
    "[rezonality][diagnostics]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path()
        / "draxul-rezonality-diagnostic-utf8";
    std::error_code ec;
    fs::remove_all(root, ec);
    REQUIRE(fs::create_directories(root));

    rezonality::DiagnosticsPublisher publisher(root,
        fs::path("project"), "utf8-contract");
    rezonality::DiagnosticState state;
    state.project_path = "project";
    state.scenegraph_path = "project/default.scenegraph";
    state.stage = "compile";
    state.severity = "error";
    constexpr size_t maximum_message_bytes = 16u * 1024u;
    state.message = std::string(maximum_message_bytes - 1u, 'a')
        + "\xC3\xA9";
    state.diagnostics.push_back({
        .path = state.scenegraph_path,
        .stage = "compile",
        .severity = "error",
        .message = "compiler\xFFoutput",
    });

    std::string error;
    REQUIRE(publisher.publish(state, error));
    auto document = read_json(publisher.path());
    CHECK(document["message"].get<std::string>()
        == std::string(maximum_message_bytes - 1u, 'a'));
    CHECK(document["diagnostics"][0]["message"].get<std::string>()
        == "compiler\xEF\xBF\xBDoutput");

    state.message = std::string(maximum_message_bytes - 2u, 'b')
        + "\xE2\x82\xAC";
    REQUIRE(publisher.publish(state, error));
    document = read_json(publisher.path());
    CHECK(document["message"].get<std::string>()
        == std::string(maximum_message_bytes - 2u, 'b'));

    state.message = std::string(maximum_message_bytes - 3u, 'c')
        + "\xF0\x9F\x9A\x80";
    REQUIRE(publisher.publish(state, error));
    document = read_json(publisher.path());
    CHECK(document["message"].get<std::string>()
        == std::string(maximum_message_bytes - 3u, 'c'));

    state.message = std::string(maximum_message_bytes - 2u, 'd')
        + "\xC3\xA9";
    REQUIRE(publisher.publish(state, error));
    document = read_json(publisher.path());
    CHECK(document["message"].get<std::string>() == state.message);

    state.message = std::string("invalid ")
        + std::string("\xF0\x28\x8C\x28", 4);
    REQUIRE(publisher.publish(state, error));
    document = read_json(publisher.path());
    CHECK(document["message"].get<std::string>().find("\xEF\xBF\xBD")
        != std::string::npos);
    CHECK_NOTHROW(document.dump());

    state.message = "recovered diagnostic";
    state.diagnostics.clear();
    REQUIRE(publisher.publish(state, error));
    document = read_json(publisher.path());
    CHECK(document["message"] == "recovered diagnostic");
    CHECK(document["diagnostics"].empty());

    REQUIRE(publisher.remove(error));
    fs::remove_all(root, ec);
    CHECK_FALSE(ec);
}

TEST_CASE("Rezonality diagnostics recover after publication failure",
    "[rezonality][diagnostics]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path()
        / "draxul-rezonality-diagnostic-recovery";
    std::error_code ec;
    fs::remove_all(root, ec);
    write_text(root, "blocks diagnostics directory creation");

    rezonality::DiagnosticsPublisher publisher(root,
        fs::path("project"), "recovery-contract");
    rezonality::DiagnosticState state;
    state.message = "still alive";
    std::string error;
    CHECK_FALSE(publisher.publish(state, error));
    CHECK_FALSE(error.empty());

    REQUIRE(fs::remove(root, ec));
    REQUIRE_FALSE(ec);
    error.clear();
    REQUIRE(publisher.publish(state, error));
    CHECK(read_json(publisher.path())["message"] == "still alive");

    REQUIRE(publisher.remove(error));
    fs::remove_all(root, ec);
    CHECK_FALSE(ec);
}

TEST_CASE("NYX panels default to full resolution and retain CRT variants",
    "[rezonality][nyx][scenegraph]")
{
    const auto shaders
        = plugin_root() / "examples" / "nyx_flight_deck" / "shaders";
    for (int panel = 0; panel < 10; ++panel)
    {
        const std::string prefix = "panel-" + std::to_string(panel);
        const std::string clean
            = read_text(shaders / (prefix + ".scenegraph"));
        const std::string crt
            = read_text(shaders / (prefix + "-crt.scenegraph"));
        CAPTURE(panel);
        CHECK(clean.find("targets: (default_color)") != std::string::npos);
        CHECK(clean.find("crt.frag") == std::string::npos);
        CHECK(clean.find("scale: (0.24, 0.24, 1.0)")
            == std::string::npos);
        CHECK(crt.find("targets: (Signal)") != std::string::npos);
        CHECK(crt.find("scale: (0.24, 0.24, 1.0)")
            != std::string::npos);
        CHECK(crt.find("fs: crt.frag") != std::string::npos);
    }

    const std::string launcher = read_text(
        plugin_root() / "examples" / "nyx_flight_deck" / "launch.ps1");
    CHECK(launcher.find("[switch]$Crt") != std::string::npos);
    CHECK(launcher.find("'-crt.scenegraph'") != std::string::npos);
}

TEST_CASE("Rezonality model assets load immutably and fail as a candidate",
    "[rezonality][integration][model]")
{
    namespace fs = std::filesystem;
    const fs::path fixture = fs::temp_directory_path()
        / "draxul-rezonality-model-contract";
    std::error_code ec;
    fs::remove_all(fixture, ec);
    REQUIRE(fs::create_directories(fixture));
    write_text(fixture / "triangle.mtl",
        "newmtl painted\n"
        "Kd 1.0 1.0 1.0\n"
        "map_Kd texture.png\n");
    write_text(fixture / "triangle.obj",
        "mtllib triangle.mtl\n"
        "o triangle\n"
        "v -1 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 0.5 1\n"
        "usemtl painted\n"
        "f 1/1 2/2 3/3\n");
    const fs::path first_texture = plugin_root() / "examples" / "default"
        / "noise.png";
    const fs::path second_texture = plugin_root() / "examples" / "pbr_robot"
        / "models" / "robot" / "textures"
        / "RobotChest_metallicRoughness.png";
    REQUIRE(fs::copy_file(first_texture, fixture / "texture.png",
        fs::copy_options::overwrite_existing));

    rezonality::ModelData first;
    std::string error;
    REQUIRE(rezonality::load_model(
        fixture / "triangle.obj", { 2.0f, 1.0f, 1.0f }, true,
        first, error));
    REQUIRE(first.vertices.size() == 3);
    CHECK(first.indices.size() == 3);
    REQUIRE_FALSE(first.materials.empty());
    CHECK(first.vertices[0].position.x == Catch::Approx(-2.0f));
    CHECK(first.vertices[2].position.y == Catch::Approx(1.0f));
    const auto first_pixels = first.materials.back().base_color.pixels;
    REQUIRE_FALSE(first_pixels.empty());
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    std::vector<uint8_t> source_pixels;
    REQUIRE(rezonality::load_rgba8_image(fixture / "texture.png",
        source_width, source_height, source_pixels, error));
    const size_t source_row_bytes = static_cast<size_t>(source_width) * 4;
    REQUIRE(first_pixels.size() == source_pixels.size());
    CHECK(std::equal(first_pixels.begin(),
        first_pixels.begin() + source_row_bytes,
        source_pixels.end() - source_row_bytes));

    REQUIRE(fs::copy_file(second_texture, fixture / "texture.png",
        fs::copy_options::overwrite_existing));
    rezonality::ModelData second;
    error.clear();
    REQUIRE(rezonality::load_model(
        fixture / "triangle.obj", { 2.0f, 1.0f, 1.0f }, false,
        second, error));
    REQUIRE_FALSE(second.materials.empty());
    CHECK(second.materials.back().base_color.pixels != first_pixels);
    source_pixels.clear();
    REQUIRE(rezonality::load_rgba8_image(fixture / "texture.png",
        source_width, source_height, source_pixels, error));
    const size_t second_row_bytes = static_cast<size_t>(source_width) * 4;
    CHECK(std::equal(second.materials.back().base_color.pixels.begin(),
        second.materials.back().base_color.pixels.begin() + second_row_bytes,
        source_pixels.begin()));
    CHECK(first.materials.back().base_color.pixels == first_pixels);

    REQUIRE(fs::remove(fixture / "texture.png"));
    rezonality::ModelData rejected;
    error.clear();
    CHECK_FALSE(rezonality::load_model(
        fixture / "triangle.obj", { 1.0f, 1.0f, 1.0f }, true,
        rejected, error));
    CHECK(error.find("texture.png") != std::string::npos);
    CHECK(rejected.vertices.empty());
    CHECK(first.materials.back().base_color.pixels == first_pixels);
    fs::remove_all(fixture, ec);
}

TEST_CASE("Rezonality bakes nonuniform and mirrored model shading bases",
    "[rezonality][integration][model]")
{
    namespace fs = std::filesystem;
    const fs::path fixture = fs::temp_directory_path()
        / "draxul-rezonality-model-basis";
    std::error_code ec;
    fs::remove_all(fixture, ec);
    REQUIRE(fs::create_directories(fixture));
    write_text(fixture / "sloped.obj",
        "o slope\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 1\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 0 1\n"
        "vn 0 -0.70710678 0.70710678\n"
        "f 1/1/1 2/2/1 3/3/1\n");
    rezonality::ModelData scaled;
    std::string error;
    REQUIRE(rezonality::load_model(fixture / "sloped.obj",
        { 1.0f, 2.0f, 3.0f }, false, scaled, error));
    REQUIRE(scaled.vertices.size() == 3);
    const auto& vertex = scaled.vertices.front();
    const glm::vec3 expected = glm::normalize(glm::vec3{
        0.0f, -0.5f, 1.0f / 3.0f });
    CHECK(glm::dot(vertex.normal, expected) > 0.999f);
    CHECK(std::abs(glm::dot(vertex.normal, vertex.tangent)) < 1e-5f);
    CHECK(std::abs(glm::dot(vertex.normal, vertex.bitangent)) < 1e-5f);
    const float handedness = glm::dot(glm::cross(vertex.normal,
        vertex.tangent), vertex.bitangent);
    CHECK(std::abs(handedness) > 0.999f);

    rezonality::ModelData mirrored;
    REQUIRE(rezonality::load_model(fixture / "sloped.obj",
        { -1.0f, 2.0f, 3.0f }, false, mirrored, error));
    REQUIRE(mirrored.vertices.size() == 3);
    const auto& reflected = mirrored.vertices.front();
    CHECK(glm::dot(glm::cross(reflected.normal, reflected.tangent),
        reflected.bitangent) * handedness < -0.999f);
    rezonality::ModelData singular;
    CHECK_FALSE(rezonality::load_model(fixture / "sloped.obj",
        { 1.0f, 0.0f, 3.0f }, false, singular, error));
    CHECK(error.find("singular scale") != std::string::npos);
    CHECK_FALSE(rezonality::load_model(fixture / "sloped.obj",
        { 1.0f, 1e-30f, 3.0f }, false, singular, error));
    fs::remove_all(fixture, ec);
    CHECK_FALSE(ec);
}

TEST_CASE("Rezonality camera orbit, dolly, and resize stay pane-local",
    "[rezonality][camera]")
{
    rezonality::Camera first;
    rezonality::Camera second;
    rezonality::camera_set_pos_lookat(
        first, { 0.0f, 0.0f, 4.0f }, { 0.0f, 0.0f, 0.0f });
    rezonality::camera_set_pos_lookat(
        second, { 0.0f, 0.0f, 4.0f }, { 0.0f, 0.0f, 0.0f });
    const auto second_position = second.position;
    rezonality::camera_orbit(first, { 20.0f, -10.0f });
    rezonality::camera_dolly(first, 0.5f);
    CHECK(first.position != second.position);
    CHECK(second.position == second_position);
    const auto wide = rezonality::camera_projection(first, 960, 360);
    const auto tall = rezonality::camera_projection(first, 360, 960);
    CHECK(wide != tall);
}
