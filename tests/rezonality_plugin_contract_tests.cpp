#include <catch2/catch_test_macros.hpp>

#include <draxul/plugin_api.h>

#include "rezonality_plugin_test_support.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Loads the built module through the exported C ABI, exactly as Draxul does.
// The suite deliberately compiles no Rezonality implementation of its own.

using namespace rezonality::plugin_test;

namespace
{

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
    write_text(fixture / "agent_include.glsl", "// contract include\n");
    const fs::path fixture_fragment = fixture / "screen.frag";
    write_text(fixture_fragment, read_text(fixture_fragment)
        + "\n#include \"agent_include.glsl\"\n");

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
    CHECK(document["schema_version"] == 3);
    CHECK(document["stage"] == "build");
    CHECK(document["severity"] == "info");
    CHECK(document["attempted_generation"] == 1);
    CHECK(document["project_path"] == canonical_fixture.generic_string());
    REQUIRE(document["active_source_files"].is_array());
    CHECK(document["active_source_file_count"] == 0);
    CHECK_FALSE(document["active_source_files_truncated"].get<bool>());
    REQUIRE(document["candidate_source_files"].is_array());
    CHECK(document["candidate_source_file_count"].get<size_t>()
        == document["candidate_source_files"].size());
    CHECK(document["candidate_source_file_count"].get<size_t>() >= 4);
    CHECK_FALSE(document["candidate_source_files_truncated"].get<bool>());
    bool found_scenegraph = false;
    bool found_fragment = false;
    bool found_include = false;
    for (const auto& source : document["candidate_source_files"])
    {
        const std::string path = source.get<std::string>();
        found_scenegraph = found_scenegraph
            || path.find("default.scenegraph") != std::string::npos;
        found_fragment = found_fragment
            || path.find("screen.frag") != std::string::npos;
        found_include = found_include
            || path.find("agent_include.glsl") != std::string::npos;
    }
    CHECK(found_scenegraph);
    CHECK(found_fragment);
    CHECK(found_include);
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

TEST_CASE("The staged Rezonality module opens and reloads international project paths",
    "[rezonality][integration][dynamic][international]")
{
    namespace fs = std::filesystem;
    // Text in this test is always UTF-8; on Windows, path::string() would use
    // the active code page instead, which cannot represent these names.
    const auto utf8 = [](const fs::path& path) {
        const std::u8string text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    };
    DynamicPluginModule module(fs::path(DRAXUL_REZONALITY_MODULE_PATH));
    const auto* api = module.api();
    REQUIRE(api != nullptr);

    const auto fixture_id = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
    const fs::path root = fs::temp_directory_path()
        / ("draxul-rezonality-international-" + std::to_string(fixture_id));
    for (const std::u8string_view name : {
             std::u8string_view(u8"caf\u00E9-proj\u00E8t"),
             std::u8string_view(u8"\u65E5\u672C\u8A9E\u30D7\u30ED\u30B8\u30A7\u30AF\u30C8"),
             std::u8string_view(u8"mixer-\U0001F39B\U0001F3B5"),
         })
    {
        const fs::path fixture = root / fs::path(name);
        const std::string name_utf8(name.begin(), name.end());
        DYNAMIC_SECTION(name_utf8)
        {
            const fs::path cache = root / ("cache-" + std::to_string(
                                               name_utf8.size()));
            std::error_code ec;
            REQUIRE(fs::create_directories(fixture));
            const fs::path canonical_fixture = fs::weakly_canonical(fixture, ec);
            REQUIRE_FALSE(ec);
            fs::copy(plugin_root() / "examples" / "simple", fixture,
                fs::copy_options::recursive
                    | fs::copy_options::overwrite_existing,
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

            const std::string directory = utf8(plugin_root());
            // No diagnostics_id: the default identity is derived from the
            // international project name.
            const std::string config = nlohmann::json{
                { "project_path", utf8(fixture) },
                { "auto_reload", false },
                { "paused", true },
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
            // Waits for generation N to settle as either a ready or a failed
            // candidate; opening and reloading must never close the host.
            const auto settle = [&](uint64_t generation) {
                const std::string ready = "ready g" + std::to_string(generation);
                const std::string failed
                    = "BUILD FAILED g" + std::to_string(generation);
                const auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::seconds(30);
                DraxulPluginTickInfoV2 tick_info{};
                tick_info.struct_size = sizeof(tick_info);
                tick_info.visible = 1;
                std::string current;
                while (std::chrono::steady_clock::now() < deadline)
                {
                    api->tick(instance, &tick_info);
                    current = presentation_status(instance, presentation);
                    if (current.find(ready) != std::string::npos
                        || current.find(failed) != std::string::npos)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                INFO(current);
                // Windows runs the narrow-argv compiler inside the project
                // with project-relative names, so these must compile too.
                CHECK(current.find(ready) != std::string::npos);
                return current;
            };
            const std::string status = settle(1);
            CHECK(status.rfind(name_utf8 + " | ", 0) == 0);
            CHECK_NOTHROW(nlohmann::json(status).dump());

            std::vector<fs::path> published;
            for (const auto& entry :
                fs::directory_iterator(cache / "diagnostics", ec))
                published.push_back(entry.path());
            REQUIRE_FALSE(ec);
            REQUIRE(published.size() == 1);
            const std::string identity = utf8(published.front().filename());
            CHECK(std::all_of(identity.begin(), identity.end(),
                [](unsigned char value) { return value < 0x80; }));
            auto document = read_json(published.front());
            CHECK(document["project_path"] == utf8(canonical_fixture));

            DraxulPluginHotReloadExtensionV2 reload{};
            REQUIRE(api->query_extension(instance,
                        DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_ID,
                        sizeof(DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_ID) - 1,
                        DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_VERSION,
                        &reload, sizeof(reload))
                != 0);
            size_t required = 0;
            REQUIRE(reload.export_json(instance, nullptr, &required));
            std::vector<char> state(required);
            size_t capacity = state.size();
            REQUIRE(reload.export_json(instance, state.data(), &capacity));
            const auto exported = nlohmann::json::parse(
                state.data(), state.data() + capacity - 1);
            CHECK(exported["project_path"] == utf8(canonical_fixture));

            REQUIRE(presentation.dispatch_action(instance,
                        "rezonality_reload", sizeof("rezonality_reload") - 1)
                != 0);
            CHECK(settle(2).rfind(name_utf8 + " | ", 0) == 0);

            api->quiesce_instance(instance);
            api->destroy_instance(instance);
            instance = api->create_instance(&create_info);
            REQUIRE(instance != nullptr);
            DraxulPluginHotReloadExtensionV2 replacement{};
            REQUIRE(api->query_extension(instance,
                        DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_ID,
                        sizeof(DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_ID) - 1,
                        DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_VERSION,
                        &replacement, sizeof(replacement))
                != 0);
            CHECK(replacement.import_json(instance, state.data(), capacity - 1,
                      reload.schema_id, reload.schema_version)
                != 0);
            api->quiesce_instance(instance);
            api->destroy_instance(instance);
        }
    }
    std::error_code ec;
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

TEST_CASE("The staged Rezonality module rejects texture feedback edits",
    "[rezonality][integration][dynamic][feedback]")
{
    namespace fs = std::filesystem;
    DynamicPluginModule module(fs::path(DRAXUL_REZONALITY_MODULE_PATH));
    const auto* api = module.api();
    REQUIRE(api != nullptr);

    const fs::path fixture = fs::temp_directory_path()
        / ("draxul-rezonality-feedback-"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    REQUIRE(fs::create_directories(fixture));
    fs::copy(plugin_root() / "examples" / "simple", fixture,
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
    // The example samples A in Composite after Pass1 writes it.
    REQUIRE(wait_for_status(*api, instance, presentation, "ready g1"));
    const auto reload_and_wait = [&](std::string_view expected) {
        REQUIRE(presentation.dispatch_action(instance,
                    "rezonality_reload", sizeof("rezonality_reload") - 1)
            != 0);
        REQUIRE(wait_for_status(*api, instance, presentation, expected));
        return presentation_status(instance, presentation);
    };

    const fs::path scenegraph = fixture / "default.scenegraph";
    const std::string scene = read_text(scenegraph);
    const auto replace = [&scene](std::string_view from, std::string_view to) {
        std::string edited = scene;
        const size_t at = edited.find(from);
        REQUIRE(at != std::string::npos);
        edited.replace(at, from.size(), to);
        return edited;
    };

    write_text(scenegraph, replace("samplers: (A)", "samplers: (!A)"));
    // Rejected while parsing, so neither backend prepares the candidate.
    const std::string history = reload_and_wait("BUILD FAILED g2");
    CHECK(history.find("default.scenegraph:17") != std::string::npos);
    CHECK(history.find("Pass 'Composite' samples '!A' (previous frame)")
        != std::string::npos);

    write_text(scenegraph,
        replace("targets: (A)", "targets: (A)\n    samplers: (A)"));
    const std::string aliased = reload_and_wait("BUILD FAILED g3");
    CHECK(aliased.find("default.scenegraph:7") != std::string::npos);
    CHECK(aliased.find("Pass 'Pass1' samples 'A' while writing it")
        != std::string::npos);

    write_text(scenegraph, scene);
    reload_and_wait("ready g4");

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
