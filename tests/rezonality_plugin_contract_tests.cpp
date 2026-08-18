#include <catch2/catch_test_macros.hpp>

#include <draxul/plugin_api.h>

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
        + std::chrono::seconds(10);
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
