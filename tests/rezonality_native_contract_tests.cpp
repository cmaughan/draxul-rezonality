#include <catch2/catch_test_macros.hpp>

#include <draxul/plugin_api.h>

#include "rezonality_plugin_test_support.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

// In-process contract: the test executable links the same compiled ABI
// adapter and native backend objects as the staged module and calls the
// exported entry point directly.

using namespace rezonality::plugin_test;

TEST_CASE("Rezonality exports a usable Draxul plugin contract",
    "[rezonality][plugin]")
{
    const auto* api = draxul_plugin_query_v2(DRAXUL_PLUGIN_ABI_VERSION);
    REQUIRE(api != nullptr);
    CHECK(api->abi_version == DRAXUL_PLUGIN_ABI_VERSION);
    CHECK(std::string_view(api->plugin_id) == "dev.draxul.rezonality");
    CHECK(std::string_view(api->display_name) == "Rezonality");
    CHECK(std::string_view(api->plugin_version) == "0.7.0");
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
        sizeof(DraxulPluginViewportV2), 0, 0, 640, 480, 1.0f, 96.0f
    };

    void* instance = api->create_instance(&create_info);
    REQUIRE(instance != nullptr);

    DraxulPluginPresentationExtensionV2 presentation{};
    REQUIRE(api->query_extension(instance,
                DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID,
                sizeof(DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID) - 1,
                DRAXUL_PLUGIN_PRESENTATION_EXTENSION_VERSION,
                &presentation, sizeof(presentation))
        != 0);

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
    write_text(fixture / "first.inc", "#include \"nested.inc\"\n");
    write_text(fixture / "nested.inc",
        "vec4 included_color() { return vec4(0,1,0,1); }\n");

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
    }
                                   .dump();
    DraxulPluginCreateInfoV2 create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.host = &host;
    create_info.plugin_id = api->plugin_id;
    create_info.plugin_directory_utf8 = directory.c_str();
    create_info.config_json = config.data();
    create_info.config_json_length = config.size();
    create_info.initial_viewport = {
        sizeof(DraxulPluginViewportV2), 0, 0, 640, 480, 1.0f, 96.0f
    };
    void* instance = api->create_instance(&create_info);
    REQUIRE(instance != nullptr);

    DraxulPluginPresentationExtensionV2 presentation{};
    REQUIRE(api->query_extension(instance,
                DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID,
                sizeof(DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID) - 1,
                DRAXUL_PLUGIN_PRESENTATION_EXTENSION_VERSION,
                &presentation, sizeof(presentation))
        != 0);
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g1"));

    write_text(fixture / "screen.frag",
        "#version 450\n"
        "layout(location=0) out vec4 fragColor;\n"
        "void main() { fragColor = vec4(1,0,0,1); }\n");
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g2"));

    const fs::path scenegraph = fixture / "default.scenegraph";
    const std::string scene = read_text(scenegraph);
    write_text(scenegraph, scene
        + "\ncamera: Incomplete { field_of_view: - }\n");
    REQUIRE(wait_for_status(*api, instance, presentation,
        "BUILD FAILED g3"));
    CHECK(presentation_status(instance, presentation).find(
        "default.scenegraph") != std::string::npos);
    write_text(scenegraph, scene);
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g4"));

    write_text(fixture / "screen.frag",
        "#version 450\n"
        "layout(location=0) out vec4 fragColor;\n"
        "void main() { fragColor = vec4(; }\n");
    REQUIRE(wait_for_status(*api, instance, presentation,
        "BUILD FAILED g5"));
    const std::string failed_status = presentation_status(instance, presentation);
    CHECK(failed_status.find("screen.frag") != std::string::npos);

    write_text(fixture / "screen.frag",
        "#version 450\n"
        "layout(location=0) out vec4 fragColor;\n"
        "void main() { fragColor = vec4(0,0,1,1); }\n");
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g6"));
    write_text(fixture / "screen.frag",
        "#version 450\n"
        "#extension GL_GOOGLE_include_directive : require\n"
        "#include \"first.inc\"\n"
        "layout(location=0) out vec4 fragColor;\n"
        "void main() { fragColor = included_color(); }\n");
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g7"));
    write_text(fixture / "nested.inc",
        "vec4 included_color() { return vec4(1,0,1,1); }\n");
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g8"));
    write_text(fixture / "nested.inc", "this is not valid GLSL\n");
    REQUIRE(wait_for_status(*api, instance, presentation,
        "BUILD FAILED g9"));
    CHECK(presentation_status(instance, presentation).find("nested.inc")
        != std::string::npos);
    write_text(fixture / "nested.inc",
        "vec4 included_color() { return vec4(0,1,0,1); }\n");
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g10"));
    CHECK(host_state.ticks.load() >= 10);

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

TEST_CASE("Rezonality compiles every staged example",
    "[rezonality][integration][inventory]")
{
    struct Example
    {
        std::string_view path;
        std::string_view scenegraph;
    };
    const auto* api = draxul_plugin_query_v2(DRAXUL_PLUGIN_ABI_VERSION);
    REQUIRE(api != nullptr);
    const std::string directory = plugin_root().string();
    for (const Example example : {
             Example{ "simple", {} },
             Example{ "default", {} },
             Example{ "blend_waves", {} },
             Example{ "deferred_shading", {} },
             Example{ "protoplanetary_disc", {} },
             Example{ "pbr_robot", {} },
             Example{ "robot2", {} },
             Example{ "ray_tracer", {} },
             Example{ "audio_spectrum_analysis", {} },
             Example{ "nyx_flight_deck/robot-crt", "default.scenegraph" },
             Example{ "nyx_flight_deck/robot-crt", "crt.scenegraph" },
         })
    {
        DYNAMIC_SECTION(example.path << " / "
                                     << (example.scenegraph.empty()
                                                ? "project default"
                                                : example.scenegraph))
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
            nlohmann::json config_json{
                { "project_path",
                    (plugin_root() / "examples" / example.path).string() },
                { "auto_reload", false },
                { "compile_debounce_ms", 25 },
            };
            if (!example.scenegraph.empty())
                config_json["scenegraph"] = example.scenegraph;
            const std::string config = config_json.dump();
            DraxulPluginCreateInfoV2 create_info{};
            create_info.struct_size = sizeof(create_info);
            create_info.host = &host;
            create_info.plugin_id = api->plugin_id;
            create_info.plugin_directory_utf8 = directory.c_str();
            create_info.config_json = config.data();
            create_info.config_json_length = config.size();
            create_info.initial_viewport = {
                sizeof(DraxulPluginViewportV2), 0, 0,
                640, 480, 1.0f, 96.0f
            };
            void* instance = api->create_instance(&create_info);
            REQUIRE(instance != nullptr);
            DraxulPluginPresentationExtensionV2 presentation{};
            REQUIRE(api->query_extension(instance,
                        DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID,
                        sizeof(DRAXUL_PLUGIN_PRESENTATION_EXTENSION_ID) - 1,
                        DRAXUL_PLUGIN_PRESENTATION_EXTENSION_VERSION,
                        &presentation, sizeof(presentation))
                != 0);
            CHECK(wait_for_status(*api, instance, presentation, "ready g1"));
            api->quiesce_instance(instance);
            api->destroy_instance(instance);
        }
    }
}
