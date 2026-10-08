#pragma once

// Host fakes and file helpers shared by the in-process native contract suite
// and the dynamic-load module suite. Header-only so each executable keeps its
// own copy without another compiled test library.

#include <catch2/catch_test_macros.hpp>

#include <draxul/plugin_api.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

namespace rezonality::plugin_test
{

struct HostState
{
    std::atomic_uint32_t ticks{ 0 };
    std::atomic_uint32_t redraws{ 0 };
    std::filesystem::path cache_path;
};

inline void request_tick(void* context)
{
    static_cast<HostState*>(context)->ticks.fetch_add(1);
}

inline void request_redraw(void* context)
{
    static_cast<HostState*>(context)->redraws.fetch_add(1);
}

inline void request_noop(void*) {}
inline void log_noop(void*, uint32_t, const char*, size_t) {}
inline int32_t query_service_noop(void*, const char*, size_t, uint32_t, void*, size_t)
{
    return 0;
}

inline int32_t get_test_path(void* context, uint32_t kind, char* buffer,
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

inline int32_t query_path_service(void* context, const char* id,
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

inline std::string presentation_status(void* instance,
    const DraxulPluginPresentationExtensionV2& presentation)
{
    DraxulPluginPresentationStateV2 state{};
    state.struct_size = sizeof(state);
    if (!presentation.get_state(instance, &state))
        return {};
    return std::string(state.status_text.data, state.status_text.length);
}

inline bool wait_for_status(const DraxulPluginApiV2& api, void* instance,
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

inline void write_text(const std::filesystem::path& path, std::string_view contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output << contents;
    REQUIRE(output.good());
}

inline std::filesystem::path plugin_root()
{
    return std::filesystem::path(DRAXUL_PROJECT_ROOT)
        / "plugins" / "rezonality";
}

inline std::string read_text(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    return { std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>() };
}

inline nlohmann::json read_json(const std::filesystem::path& path)
{
    return nlohmann::json::parse(read_text(path));
}

} // namespace rezonality::plugin_test
