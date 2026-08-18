#include <catch2/catch_test_macros.hpp>

#include <draxul/plugin_api.h>

#include <string_view>

namespace
{

void request_noop(void*) {}
void log_noop(void*, uint32_t, const char*, size_t) {}
int32_t query_service_noop(void*, const char*, size_t, uint32_t, void*, size_t)
{
    return 0;
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

    DraxulPluginHostApiV2 host{};
    host.struct_size = sizeof(host);
    host.abi_version = DRAXUL_PLUGIN_ABI_VERSION;
    host.request_redraw = &request_noop;
    host.request_tick = &request_noop;
    host.notify_presentation_changed = &request_noop;
    host.log = &log_noop;
    host.query_service = &query_service_noop;

    DraxulPluginCreateInfoV2 create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.host = &host;
    create_info.plugin_id = api->plugin_id;
    create_info.plugin_directory_utf8 = ".";
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
