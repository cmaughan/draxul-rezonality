#include <catch2/catch_test_macros.hpp>

#include <draxul/plugin_api.h>

#include "audio_analysis.h"
#include "camera.h"
#include "image_loader.h"
#include "model_loader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{

struct HostState
{
    std::atomic_uint32_t ticks{ 0 };
    std::atomic_uint32_t redraws{ 0 };
    std::filesystem::path cache_path;
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

int32_t get_test_path(void* context, uint32_t kind, char* buffer,
    size_t* in_out_size)
{
    auto* state = static_cast<HostState*>(context);
    if (!state || !in_out_size || kind != DRAXUL_PLUGIN_PATH_CACHE
        || state->cache_path.empty())
        return 0;
    const std::string value = state->cache_path.string();
    const size_t required = value.size() + 1;
    if (!buffer)
    {
        *in_out_size = required;
        return 1;
    }
    if (*in_out_size < required)
    {
        *in_out_size = required;
        return 0;
    }
    std::memcpy(buffer, value.c_str(), required);
    *in_out_size = required;
    return 1;
}

int32_t query_path_service(void* context, const char* id,
    size_t id_length, uint32_t version, void* table, size_t table_size)
{
    if (!context || !id || !table
        || std::string_view(id, id_length) != DRAXUL_PLUGIN_PATH_SERVICE_ID
        || version != DRAXUL_PLUGIN_PATH_SERVICE_VERSION
        || table_size < sizeof(DraxulPluginPathServiceV2))
        return 0;
    auto* service = static_cast<DraxulPluginPathServiceV2*>(table);
    *service = { sizeof(*service), DRAXUL_PLUGIN_PATH_SERVICE_VERSION,
        context, &get_test_path };
    return 1;
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

class DynamicPluginModule
{
public:
    explicit DynamicPluginModule(const std::filesystem::path& path)
    {
#if defined(_WIN32)
        handle_ = LoadLibraryW(path.c_str());
        if (handle_)
            query_ = reinterpret_cast<Query>(
                GetProcAddress(handle_, "draxul_plugin_query_v2"));
#else
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle_)
            query_ = reinterpret_cast<Query>(
                dlsym(handle_, "draxul_plugin_query_v2"));
#endif
    }

    ~DynamicPluginModule()
    {
#if defined(_WIN32)
        if (handle_)
            FreeLibrary(handle_);
#else
        if (handle_)
            dlclose(handle_);
#endif
    }

    DynamicPluginModule(const DynamicPluginModule&) = delete;
    DynamicPluginModule& operator=(const DynamicPluginModule&) = delete;

    const DraxulPluginApiV2* api() const
    {
        return query_ ? query_(DRAXUL_PLUGIN_ABI_VERSION) : nullptr;
    }

private:
    using Query = const DraxulPluginApiV2* (*)(uint32_t);
#if defined(_WIN32)
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
    Query query_ = nullptr;
};

} // namespace

TEST_CASE("Rezonality Metal model passes test and write depth",
    "[rezonality][metal][depth]")
{
    const std::string source
        = read_text(plugin_root() / "src" / "rezonality_plugin.cpp");
    CHECK(source.find("depthCompareFunction = MTLCompareFunctionLessEqual")
        != std::string::npos);
    CHECK(source.find("depthWriteEnabled = YES") != std::string::npos);
    CHECK(source.find("scene_pass.model_index && scene_pass.has_depth")
        != std::string::npos);
    CHECK(source.find("[encoder setDepthStencilState:")
        != std::string::npos);
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

TEST_CASE("Rezonality synthetic audio produces a stable stereo analysis texture",
    "[rezonality][audio]")
{
    rezonality::AudioOptions options;
    options.source = rezonality::AudioOptions::Source::Synthetic;
    rezonality::AudioAnalyzer analyzer(options);

    const auto first = analyzer.frame();
    const auto second = analyzer.frame();
    REQUIRE(first.rgba.size()
        == rezonality::AudioTextureFrame::width
            * rezonality::AudioTextureFrame::height * 4);
    CHECK(first.generation == 1);
    CHECK(first.status == "audio synthetic fixture");
    CHECK(first.rgba == second.rgba);
    CHECK(std::any_of(first.rgba.begin(), first.rgba.end(),
        [](float value) { return value > 0.1f && value < 0.99f; }));

    analyzer.set_visible(false);
    CHECK(analyzer.frame().rgba == first.rgba);
    analyzer.set_visible(true);
    CHECK(analyzer.frame().rgba == first.rgba);

    options.source = rezonality::AudioOptions::Source::Silent;
    rezonality::AudioAnalyzer silent(options);
    const auto fallback = silent.frame();
    CHECK(fallback.generation == 1);
    CHECK(fallback.status.find("audio unavailable") != std::string::npos);
    CHECK(fallback.rgba.size() == first.rgba.size());
    CHECK(std::all_of(fallback.rgba.begin(), fallback.rgba.end(),
        [](float value) { return value == 0.0f || value == 1.0f; }));
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

    write_text(fixture / "screen.frag",
        "#version 450\n"
        "layout(location=0) out vec4 fragColor;\n"
        "void main() { fragColor = vec4(; }\n");
    REQUIRE(wait_for_status(*api, instance, presentation,
        "BUILD FAILED g3"));
    const std::string failed_status = presentation_status(instance, presentation);
    CHECK(failed_status.find("screen.frag") != std::string::npos);

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

TEST_CASE("The staged Rezonality module publishes agent diagnostics and hands off state",
    "[rezonality][integration][dynamic][agent]")
{
    namespace fs = std::filesystem;
    DynamicPluginModule module(fs::path(DRAXUL_REZONALITY_MODULE_PATH));
    const auto* api = module.api();
    REQUIRE(api != nullptr);

    const auto fixture_id = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
    const fs::path root = fs::temp_directory_path()
        / ("draxul-rezonality-agent-workflow-"
            + std::to_string(fixture_id));
    const fs::path fixture = root / "project";
    const fs::path cache = root / "cache";
    std::error_code ec;
    REQUIRE(fs::create_directories(fixture));
    const fs::path canonical_fixture = fs::weakly_canonical(fixture, ec);
    REQUIRE_FALSE(ec);
    fs::copy(plugin_root() / "examples" / "simple", fixture,
        fs::copy_options::recursive | fs::copy_options::overwrite_existing,
        ec);
    REQUIRE_FALSE(ec);

    HostState host_state;
    host_state.cache_path = cache;
    DraxulPluginHostApiV2 host{};
    host.struct_size = sizeof(host);
    host.abi_version = DRAXUL_PLUGIN_ABI_VERSION;
    host.host_context = &host_state;
    host.request_redraw = &request_redraw;
    host.request_tick = &request_tick;
    host.notify_presentation_changed = &request_noop;
    host.log = &log_noop;
    host.query_service = &query_path_service;

    const std::string directory = plugin_root().string();
    const std::string config = nlohmann::json{
        { "project_path", fixture.string() },
        { "auto_reload", false },
        { "paused", true },
        { "compile_debounce_ms", 25 },
        { "diagnostics_id", "agent-workflow" },
    }.dump();
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
    CHECK(presentation_status(instance, presentation).find("project | ready g1")
        != std::string::npos);

    const fs::path diagnostics
        = cache / "diagnostics" / "agent-workflow.json";
    REQUIRE(fs::exists(diagnostics));
    auto document = read_json(diagnostics);
    CHECK(document["schema_version"] == 2);
    CHECK(document["stage"] == "build");
    CHECK(document["severity"] == "info");
    CHECK(document["attempted_generation"] == 1);
    CHECK(document["project_path"] == canonical_fixture.generic_string());
    REQUIRE(document["diagnostics"].is_array());
    REQUIRE(document["diagnostics"].size() == 1);

    const auto reload_and_wait = [&](std::string_view expected) {
        REQUIRE(presentation.dispatch_action(instance,
                    "rezonality_reload", sizeof("rezonality_reload") - 1)
            != 0);
        REQUIRE(wait_for_status(*api, instance, presentation, expected));
    };
    const fs::path shader_path = fixture / "screen.frag";
    const std::string shader = read_text(shader_path);
    const fs::path second_shader_path = fixture / "copy.vert";
    const std::string second_shader = read_text(second_shader_path);
    write_text(shader_path, shader + "\n// agent valid edit\n");
    reload_and_wait("ready g2");
    write_text(shader_path, shader + "\nthis is not valid GLSL\n");
    write_text(second_shader_path,
        second_shader + "\nthis is also not valid GLSL\n");
    reload_and_wait("BUILD FAILED g3");
    CHECK(presentation_status(instance, presentation).find(" more")
        != std::string::npos);
    document = read_json(diagnostics);
    CHECK(document["stage"] == "compile");
    CHECK(document["severity"] == "error");
    CHECK(document["attempted_generation"] == 3);
    CHECK(document["path"].get<std::string>().find("screen.frag")
        != std::string::npos);
    CHECK(document["line"].get<int>() > 0);
    CHECK_FALSE(document["message"].get<std::string>().empty());
    REQUIRE(document["diagnostics"].is_array());
    REQUIRE(document["diagnostics"].size() >= 2);
    CHECK(document["diagnostic_count"].get<size_t>()
        == document["diagnostics"].size());
    CHECK_FALSE(document["diagnostics_truncated"].get<bool>());
    bool found_screen = false;
    bool found_copy = false;
    for (const auto& diagnostic : document["diagnostics"])
    {
        const std::string path = diagnostic["path"].get<std::string>();
        found_screen = found_screen
            || path.find("screen.frag") != std::string::npos;
        found_copy = found_copy
            || path.find("copy.vert") != std::string::npos;
        CHECK(diagnostic["stage"] == "compile");
        CHECK_FALSE(diagnostic["message"].get<std::string>().empty());
    }
    CHECK(found_screen);
    CHECK(found_copy);
    write_text(shader_path, shader);
    write_text(second_shader_path, second_shader);
    reload_and_wait("ready g4");
    document = read_json(diagnostics);
    CHECK(document["stage"] == "build");
    CHECK(document["severity"] == "info");
    CHECK(document["attempted_generation"] == 4);

    DraxulPluginHotReloadExtensionV2 reload{};
    REQUIRE(api->query_extension(instance,
                DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_ID,
                sizeof(DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_ID) - 1,
                DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_VERSION,
                &reload, sizeof(reload))
        != 0);
    CHECK(std::string_view(reload.schema_id)
        == "dev.draxul.rezonality.state");
    const std::string imported = nlohmann::json{
        { "project_path", canonical_fixture.generic_string() },
        { "scenegraph", "default.scenegraph" },
        { "time_seconds", 12.5 },
        { "paused", false },
        { "camera_position", { 2.0, 3.0, 7.0 } },
        { "camera_focal_point", { 0.5, 0.25, 0.0 } },
    }.dump();
    REQUIRE(reload.import_json(instance, imported.data(), imported.size(),
        reload.schema_id, reload.schema_version));
    size_t required = 0;
    REQUIRE(reload.export_json(instance, nullptr, &required));
    std::vector<char> state(required);
    size_t capacity = state.size();
    REQUIRE(reload.export_json(instance, state.data(), &capacity));
    const auto exported = nlohmann::json::parse(
        state.data(), state.data() + capacity - 1);
    CHECK(exported["time_seconds"].get<double>() == Catch::Approx(12.5));
    CHECK(exported["paused"] == false);
    CHECK(exported["camera_position"][0].get<float>()
        == Catch::Approx(2.0f));

    api->quiesce_instance(instance);
    api->destroy_instance(instance);
    CHECK_FALSE(fs::exists(diagnostics));
    instance = api->create_instance(&create_info);
    REQUIRE(instance != nullptr);
    DraxulPluginHotReloadExtensionV2 replacement_reload{};
    REQUIRE(api->query_extension(instance,
                DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_ID,
                sizeof(DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_ID) - 1,
                DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_VERSION,
                &replacement_reload, sizeof(replacement_reload))
        != 0);
    REQUIRE(replacement_reload.import_json(instance, state.data(), capacity - 1,
        reload.schema_id, reload.schema_version));
    required = 0;
    REQUIRE(replacement_reload.export_json(instance, nullptr, &required));
    std::vector<char> replacement_state(required);
    capacity = replacement_state.size();
    REQUIRE(replacement_reload.export_json(
        instance, replacement_state.data(), &capacity));
    const auto replacement = nlohmann::json::parse(
        replacement_state.data(), replacement_state.data() + capacity - 1);
    CHECK(replacement["time_seconds"].get<double>()
        == Catch::Approx(12.5));
    CHECK(replacement["paused"] == false);
    CHECK(replacement["camera_focal_point"][1].get<float>()
        == Catch::Approx(0.25f));

    api->quiesce_instance(instance);
    api->destroy_instance(instance);
    fs::remove_all(root, ec);
    CHECK_FALSE(ec);
}

TEST_CASE("The staged Rezonality module survives real PBR project edits",
    "[rezonality][integration][dynamic][pbr]")
{
    namespace fs = std::filesystem;
    DynamicPluginModule module(fs::path(DRAXUL_REZONALITY_MODULE_PATH));
    const auto* api = module.api();
    REQUIRE(api != nullptr);
    REQUIRE(std::string_view(api->plugin_id) == "dev.draxul.rezonality");
    REQUIRE(std::string_view(api->plugin_version) == "0.7.0");

    const auto fixture_id = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
    const fs::path fixture = fs::temp_directory_path()
        / ("draxul-rezonality-pbr-edit-smoke-"
            + std::to_string(fixture_id));
    std::error_code ec;
    REQUIRE(fs::create_directories(fixture));
    fs::copy(plugin_root() / "examples" / "pbr_robot", fixture,
        fs::copy_options::recursive | fs::copy_options::overwrite_existing,
        ec);
    REQUIRE_FALSE(ec);

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
        { "auto_reload", false },
        { "paused", true },
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
        sizeof(DraxulPluginViewportV2), 0, 0, 960, 640, 1.0f, 96.0f
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

    const auto reload_and_wait = [&](std::string_view expected) {
        REQUIRE(presentation.dispatch_action(instance,
                    "rezonality_reload", sizeof("rezonality_reload") - 1)
            != 0);
        REQUIRE(wait_for_status(*api, instance, presentation, expected));
    };
    const fs::path shader_path = fixture / "pbr.frag";
    const std::string shader = read_text(shader_path);
    write_text(shader_path, shader + "\n// valid live shader edit\n");
    reload_and_wait("ready g2");
    write_text(shader_path, shader + "\nthis is not valid GLSL\n");
    reload_and_wait("BUILD FAILED g3");
    CHECK(presentation_status(instance, presentation).find("pbr.frag")
        != std::string::npos);
    write_text(shader_path, shader);
    reload_and_wait("ready g4");

    const fs::path scene_path = fixture / "default.scenegraph";
    const std::string scene = read_text(scene_path);
    std::string broken_scene = scene;
    const size_t model_reference = broken_scene.rfind("model: robot");
    REQUIRE(model_reference != std::string::npos);
    broken_scene.replace(model_reference, sizeof("model: robot") - 1,
        "model: missing_robot");
    write_text(scene_path, broken_scene);
    reload_and_wait("BUILD FAILED g5");
    CHECK(presentation_status(instance, presentation).find("missing_robot")
        != std::string::npos);
    write_text(scene_path, scene + "\n// valid live scene edit\n");
    reload_and_wait("ready g6");

    const fs::path texture = fixture / "models" / "robot" / "textures"
        / "RobotChest_baseColor.jpeg";
    const fs::path hidden_texture = texture.string() + ".missing";
    fs::rename(texture, hidden_texture, ec);
    REQUIRE_FALSE(ec);
    reload_and_wait("BUILD FAILED g7");
    CHECK(presentation_status(instance, presentation).find("RobotChest_baseColor.jpeg")
        != std::string::npos);
    fs::rename(hidden_texture, texture, ec);
    REQUIRE_FALSE(ec);
    write_text(scene_path, scene);
    reload_and_wait("ready g8");

    api->quiesce_instance(instance);
    api->destroy_instance(instance);
    fs::remove_all(fixture, ec);
    CHECK_FALSE(ec);
}

TEST_CASE("The staged Rezonality module rejects and repairs ray candidates",
    "[rezonality][integration][dynamic][ray]")
{
    namespace fs = std::filesystem;
    DynamicPluginModule module(fs::path(DRAXUL_REZONALITY_MODULE_PATH));
    const auto* api = module.api();
    REQUIRE(api != nullptr);

    const auto fixture_id = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
    const fs::path fixture = fs::temp_directory_path()
        / ("draxul-rezonality-ray-edit-smoke-"
            + std::to_string(fixture_id));
    std::error_code ec;
    REQUIRE(fs::create_directories(fixture));
    fs::copy(plugin_root() / "examples" / "ray_tracer", fixture,
        fs::copy_options::recursive | fs::copy_options::overwrite_existing,
        ec);
    REQUIRE_FALSE(ec);

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
        { "auto_reload", false },
        { "paused", true },
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
        sizeof(DraxulPluginViewportV2), 0, 0, 960, 640, 1.0f, 96.0f
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
    const auto reload_and_wait = [&](std::string_view expected) {
        REQUIRE(presentation.dispatch_action(instance,
                    "rezonality_reload", sizeof("rezonality_reload") - 1)
            != 0);
        REQUIRE(wait_for_status(*api, instance, presentation, expected));
    };

    const fs::path raygen_path = fixture / "rt_gen.rgen";
    const std::string raygen = read_text(raygen_path);
    write_text(raygen_path, raygen + "\n// valid ray shader edit\n");
    reload_and_wait("ready g2");
    write_text(raygen_path, raygen + "\nthis is not valid GLSL\n");
    reload_and_wait("BUILD FAILED g3");
    CHECK(presentation_status(instance, presentation).find("rt_gen.rgen")
        != std::string::npos);
    write_text(raygen_path, raygen);
    reload_and_wait("ready g4");

    const fs::path model = fixture / "cornell-box.obj";
    const fs::path hidden_model = model.string() + ".missing";
    fs::rename(model, hidden_model, ec);
    REQUIRE_FALSE(ec);
    reload_and_wait("BUILD FAILED g5");
    CHECK(presentation_status(instance, presentation).find("cornell-box.obj")
        != std::string::npos);
    fs::rename(hidden_model, model, ec);
    REQUIRE_FALSE(ec);
    reload_and_wait("ready g6");

    api->quiesce_instance(instance);
    api->destroy_instance(instance);
    fs::remove_all(fixture, ec);
    CHECK_FALSE(ec);
}
