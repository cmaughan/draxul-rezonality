#include <catch2/catch_test_macros.hpp>

#include <draxul/plugin_api.h>

#include "camera.h"
#include "model_loader.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

namespace
{

struct HostState
{
    std::atomic_uint32_t ticks{ 0 };
    std::atomic_uint32_t redraws{ 0 };
};

void request_tick(void* context)
{
    static_cast<HostState*>(context)->ticks.fetch_add(1);
}

void request_redraw(void* context)
{
    static_cast<HostState*>(context)->redraws.fetch_add(1);
}

void request_noop(void*) {}
void log_noop(void*, uint32_t, const char*, size_t) {}
int32_t query_service_noop(void*, const char*, size_t, uint32_t, void*, size_t)
{
    return 0;
}

std::string presentation_status(void* instance,
    const DraxulPluginPresentationExtensionV2& presentation)
{
    DraxulPluginPresentationStateV2 state{};
    state.struct_size = sizeof(state);
    if (!presentation.get_state(instance, &state))
        return {};
    return std::string(state.status_text.data, state.status_text.length);
}

bool wait_for_status(const DraxulPluginApiV2& api, void* instance,
    const DraxulPluginPresentationExtensionV2& presentation,
    std::string_view expected)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(30);
    DraxulPluginTickInfoV2 tick_info{};
    tick_info.struct_size = sizeof(tick_info);
    tick_info.visible = 1;
    while (std::chrono::steady_clock::now() < deadline)
    {
        api.tick(instance, &tick_info);
        if (presentation_status(instance, presentation).find(expected)
            != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

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

} // namespace

TEST_CASE("Rezonality exports a usable Draxul plugin contract",
    "[rezonality][plugin]")
{
    const auto* api = draxul_plugin_query_v2(DRAXUL_PLUGIN_ABI_VERSION);
    REQUIRE(api != nullptr);
    CHECK(api->abi_version == DRAXUL_PLUGIN_ABI_VERSION);
    CHECK(std::string_view(api->plugin_id) == "dev.draxul.rezonality");
    CHECK(std::string_view(api->display_name) == "Rezonality");
    CHECK(std::string_view(api->plugin_version) == "0.4.0");
#if defined(__APPLE__)
    CHECK(api->supported_backends == DRAXUL_PLUGIN_BACKEND_METAL);
#else
    CHECK(api->supported_backends == DRAXUL_PLUGIN_BACKEND_VULKAN);
#endif

    HostState host_state;
    DraxulPluginHostApiV2 host{};
    host.struct_size = sizeof(host);
    host.abi_version = DRAXUL_PLUGIN_ABI_VERSION;
    host.host_context = &host_state;
    host.request_redraw = &request_redraw;
    host.request_tick = &request_tick;
    host.notify_presentation_changed = &request_noop;
    host.log = &log_noop;
    host.query_service = &query_service_noop;

    DraxulPluginCreateInfoV2 create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.host = &host;
    create_info.plugin_id = api->plugin_id;
    const std::string directory = plugin_root().string();
    create_info.plugin_directory_utf8 = directory.c_str();
    create_info.config_json = "{}";
    create_info.config_json_length = 2;
    create_info.initial_viewport = {
        sizeof(DraxulPluginViewportV2), 0, 0, 640, 480, 1.0f, 96.0f };

    void* instance = api->create_instance(&create_info);
    REQUIRE(instance != nullptr);

    DraxulPluginPresentationExtensionV2 presentation{};
    REQUIRE(api->query_extension(instance,
        DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID,
        sizeof(DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID) - 1,
        DRAXUL_PLUGIN_PRESENTATION_EXTENSION_VERSION,
        &presentation, sizeof(presentation)) != 0);

    DraxulPluginPresentationStateV2 state{};
    state.struct_size = sizeof(state);
    REQUIRE(presentation.get_state(instance, &state) != 0);
    CHECK(std::string_view(state.display_name.data,
              state.display_name.length)
        == "Rezonality");
    CHECK(state.content_ready == 1);
    CHECK(wait_for_status(*api, instance, presentation, "ready g1"));
    REQUIRE(presentation.action_count(instance) == 1);
    CHECK(presentation.dispatch_action(instance,
              "rezonality_reload", sizeof("rezonality_reload") - 1)
        != 0);

    DraxulPluginTickInfoV2 tick_info{};
    tick_info.struct_size = sizeof(tick_info);
    tick_info.visible = 1;
    const auto tick = api->tick(instance, &tick_info);
    CHECK(tick.ok == 1);
    CHECK(tick.next_tick_delay_ns == DRAXUL_PLUGIN_NO_DEADLINE);

    DraxulPluginInputEventV2 pause{};
    pause.struct_size = sizeof(pause);
    pause.kind = DRAXUL_PLUGIN_INPUT_KEY;
    pause.logical_key = 32;
    pause.pressed = 1;
    CHECK(api->handle_input(instance, &pause) == 1);
    CHECK(presentation_status(instance, presentation).find("paused")
        != std::string::npos);
    CHECK(api->handle_input(instance, &pause) == 1);
    CHECK(presentation_status(instance, presentation).find("paused")
        == std::string::npos);

    const uint32_t redraws_before_camera = host_state.redraws.load();
    DraxulPluginInputEventV2 orbit{};
    orbit.struct_size = sizeof(orbit);
    orbit.kind = DRAXUL_PLUGIN_INPUT_POINTER_MOVE;
    orbit.buttons = 1;
    orbit.delta_x = 12.0f;
    orbit.delta_y = -4.0f;
    CHECK(api->handle_input(instance, &orbit) == 1);
    DraxulPluginInputEventV2 dolly{};
    dolly.struct_size = sizeof(dolly);
    dolly.kind = DRAXUL_PLUGIN_INPUT_WHEEL;
    dolly.delta_y = 1.0f;
    CHECK(api->handle_input(instance, &dolly) == 1);
    CHECK(host_state.redraws.load() >= redraws_before_camera + 2);

    DraxulPluginViewportV2 resized{
        sizeof(DraxulPluginViewportV2), 0, 0, 960, 360, 1.5f, 144.0f
    };
    api->set_viewport(instance, &resized);

    api->quiesce_instance(instance);
    CHECK(api->tick(instance, &tick_info).ok == 0);
    api->destroy_instance(instance);
}

TEST_CASE("Rezonality watches valid, broken, and repaired shader edits",
    "[rezonality][integration][reload]")
{
    namespace fs = std::filesystem;
    const auto* api = draxul_plugin_query_v2(DRAXUL_PLUGIN_ABI_VERSION);
    REQUIRE(api != nullptr);

    const fs::path fixture = fs::temp_directory_path()
        / "draxul-rezonality-live-edit-contract";
    std::error_code ec;
    fs::remove_all(fixture, ec);
    REQUIRE(fs::create_directories(fixture));
    write_text(fixture / "default.scenegraph",
        "pass: Main { geometry: background { path: screen_rect "
        "vs: screen.vert fs: screen.frag } }\n");
    write_text(fixture / "screen.vert",
        "#version 450\n"
        "layout(location=0) in vec4 inPos;\n"
        "void main() { gl_Position = inPos; }\n");
    write_text(fixture / "screen.frag",
        "#version 450\n"
        "layout(location=0) out vec4 fragColor;\n"
        "void main() { fragColor = vec4(1,1,1,1); }\n");

    HostState host_state;
    DraxulPluginHostApiV2 host{};
    host.struct_size = sizeof(host);
    host.abi_version = DRAXUL_PLUGIN_ABI_VERSION;
    host.host_context = &host_state;
    host.request_redraw = &request_redraw;
    host.request_tick = &request_tick;
    host.notify_presentation_changed = &request_noop;
    host.log = &log_noop;
    host.query_service = &query_service_noop;

    const std::string directory = plugin_root().string();
    const std::string config = nlohmann::json{
        { "project_path", fixture.string() },
        { "compile_debounce_ms", 25 },
    }.dump();
    DraxulPluginCreateInfoV2 create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.host = &host;
    create_info.plugin_id = api->plugin_id;
    create_info.plugin_directory_utf8 = directory.c_str();
    create_info.config_json = config.data();
    create_info.config_json_length = config.size();
    create_info.initial_viewport = {
        sizeof(DraxulPluginViewportV2), 0, 0, 640, 480, 1.0f, 96.0f };
    void* instance = api->create_instance(&create_info);
    REQUIRE(instance != nullptr);

    DraxulPluginPresentationExtensionV2 presentation{};
    REQUIRE(api->query_extension(instance,
        DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID,
        sizeof(DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID) - 1,
        DRAXUL_PLUGIN_PRESENTATION_EXTENSION_VERSION,
        &presentation, sizeof(presentation)) != 0);
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g1"));

    write_text(fixture / "screen.frag",
        "#version 450\n"
        "layout(location=0) out vec4 fragColor;\n"
        "void main() { fragColor = vec4(1,0,0,1); }\n");
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g2"));

    write_text(fixture / "screen.frag",
        "#version 450\n"
        "layout(location=0) out vec4 fragColor;\n"
        "void main() { fragColor = vec4(; }\n");
    REQUIRE(wait_for_status(*api, instance, presentation, "error g3"));
    CHECK(presentation_status(instance, presentation).find("screen.frag")
        != std::string::npos);

    write_text(fixture / "screen.frag",
        "#version 450\n"
        "layout(location=0) out vec4 fragColor;\n"
        "void main() { fragColor = vec4(0,0,1,1); }\n");
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g4"));
    CHECK(host_state.ticks.load() >= 4);

    api->set_visible(instance, 0);
    DraxulPluginTickInfoV2 tick_info{};
    tick_info.struct_size = sizeof(tick_info);
    tick_info.visible = 0;
    CHECK(api->tick(instance, &tick_info).next_tick_delay_ns
        == DRAXUL_PLUGIN_NO_DEADLINE);
    api->quiesce_instance(instance);
    api->destroy_instance(instance);
    fs::remove_all(fixture, ec);
}

TEST_CASE("Rezonality compiles every staged multipass example",
    "[rezonality][integration][multipass]")
{
    const auto* api = draxul_plugin_query_v2(DRAXUL_PLUGIN_ABI_VERSION);
    REQUIRE(api != nullptr);
    const std::string directory = plugin_root().string();
    for (const std::string_view example : { "default", "blend_waves",
             "deferred_shading", "protoplanetary_disc", "pbr_robot" })
    {
        DYNAMIC_SECTION(example)
        {
            HostState host_state;
            DraxulPluginHostApiV2 host{};
            host.struct_size = sizeof(host);
            host.abi_version = DRAXUL_PLUGIN_ABI_VERSION;
            host.host_context = &host_state;
            host.request_redraw = &request_redraw;
            host.request_tick = &request_tick;
            host.notify_presentation_changed = &request_noop;
            host.log = &log_noop;
            host.query_service = &query_service_noop;
            const std::string config = nlohmann::json{
                { "project_path",
                    (plugin_root() / "examples" / example).string() },
                { "auto_reload", false },
                { "compile_debounce_ms", 25 },
            }.dump();
            DraxulPluginCreateInfoV2 create_info{};
            create_info.struct_size = sizeof(create_info);
            create_info.host = &host;
            create_info.plugin_id = api->plugin_id;
            create_info.plugin_directory_utf8 = directory.c_str();
            create_info.config_json = config.data();
            create_info.config_json_length = config.size();
            create_info.initial_viewport = {
                sizeof(DraxulPluginViewportV2), 0, 0,
                640, 480, 1.0f, 96.0f };
            void* instance = api->create_instance(&create_info);
            REQUIRE(instance != nullptr);
            DraxulPluginPresentationExtensionV2 presentation{};
            REQUIRE(api->query_extension(instance,
                DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID,
                sizeof(DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID) - 1,
                DRAXUL_PLUGIN_PRESENTATION_EXTENSION_VERSION,
                &presentation, sizeof(presentation)) != 0);
            CHECK(wait_for_status(*api, instance, presentation, "ready g1"));
            api->quiesce_instance(instance);
            api->destroy_instance(instance);
        }
    }
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
        fixture / "triangle.obj", { 2.0f, 1.0f, 1.0f }, first, error));
    REQUIRE(first.vertices.size() == 3);
    CHECK(first.indices.size() == 3);
    REQUIRE_FALSE(first.materials.empty());
    CHECK(first.vertices[0].position.x == Catch::Approx(-2.0f));
    const auto first_pixels = first.materials.back().base_color.pixels;
    REQUIRE_FALSE(first_pixels.empty());

    REQUIRE(fs::copy_file(second_texture, fixture / "texture.png",
        fs::copy_options::overwrite_existing));
    rezonality::ModelData second;
    error.clear();
    REQUIRE(rezonality::load_model(
        fixture / "triangle.obj", { 2.0f, 1.0f, 1.0f }, second, error));
    REQUIRE_FALSE(second.materials.empty());
    CHECK(second.materials.back().base_color.pixels != first_pixels);
    CHECK(first.materials.back().base_color.pixels == first_pixels);

    REQUIRE(fs::remove(fixture / "texture.png"));
    rezonality::ModelData rejected;
    error.clear();
    CHECK_FALSE(rezonality::load_model(
        fixture / "triangle.obj", { 1.0f, 1.0f, 1.0f }, rejected, error));
    CHECK(error.find("texture.png") != std::string::npos);
    CHECK(rejected.vertices.empty());
    CHECK(first.materials.back().base_color.pixels == first_pixels);
    fs::remove_all(fixture, ec);
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
