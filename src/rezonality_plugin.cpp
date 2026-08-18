#include <draxul/plugin_adapter.h>
#include <draxul/plugin_api.h>

#include <string>
#include <string_view>

namespace
{

using draxul::plugin_support::render_result;
using draxul::plugin_support::tick_result;

constexpr const char* kPluginId = "dev.draxul.rezonality";

struct RezonalityInstance
{
    const DraxulPluginHostApiV2* host = nullptr;
    DraxulPluginViewportV2 viewport{};
    bool visible = true;
    bool focused = false;
    bool quiesced = false;
    std::string status = "Port scaffold ready; live renderer pending";
};

void notify_presentation(RezonalityInstance* instance)
{
    if (instance && instance->host
        && instance->host->notify_presentation_changed)
    {
        instance->host->notify_presentation_changed(
            instance->host->host_context);
    }
}

void* create_instance(const DraxulPluginCreateInfoV2* info)
{
    if (!info || info->struct_size < sizeof(DraxulPluginCreateInfoV2)
        || !info->host
        || info->host->struct_size < sizeof(DraxulPluginHostApiV2))
        return nullptr;

    auto* instance = new RezonalityInstance;
    instance->host = info->host;
    instance->viewport = info->initial_viewport;
    return instance;
}

void quiesce_instance(void* opaque)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (instance)
        instance->quiesced = true;
}

void destroy_instance(void* opaque)
{
    delete static_cast<RezonalityInstance*>(opaque);
}

void set_viewport(void* opaque, const DraxulPluginViewportV2* viewport)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (instance && viewport
        && viewport->struct_size >= sizeof(DraxulPluginViewportV2))
        instance->viewport = *viewport;
}

void set_visible(void* opaque, int32_t visible)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (instance)
        instance->visible = visible != 0;
}

void set_focused(void* opaque, int32_t focused)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (instance)
        instance->focused = focused != 0;
}

int32_t handle_input(void*, const DraxulPluginInputEventV2*)
{
    return 0;
}

DraxulPluginTickResultV2 tick(void* opaque,
    const DraxulPluginTickInfoV2*)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    return tick_result(instance && !instance->quiesced,
        DRAXUL_PLUGIN_NO_DEADLINE);
}

DraxulPluginRenderResultV2 render_vulkan(void* opaque,
    const DraxulPluginVulkanFrameV2*)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    return render_result(instance && !instance->quiesced);
}

DraxulPluginRenderResultV2 render_metal(void* opaque,
    const DraxulPluginMetalFrameV2*)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    return render_result(instance && !instance->quiesced);
}

int32_t get_presentation_state(void* opaque,
    DraxulPluginPresentationStateV2* state)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !state
        || state->struct_size < sizeof(DraxulPluginPresentationStateV2))
        return 0;

    *state = {};
    state->struct_size = sizeof(*state);
    state->display_name = { "Rezonality", 10 };
    state->status_text = {
        instance->status.data(), instance->status.size() };
    state->background_red = 0.025f;
    state->background_green = 0.035f;
    state->background_blue = 0.055f;
    state->background_alpha = 1.0f;
    state->content_ready = instance->quiesced ? 0 : 1;
    state->mouse_cursor = DRAXUL_PLUGIN_CURSOR_DEFAULT;
    return 1;
}

int32_t dispatch_action(void* opaque, const char* action,
    size_t action_length)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !action
        || std::string_view(action, action_length)
            != "rezonality_reload")
        return 0;

    instance->status = "Reload requested; live renderer port pending";
    notify_presentation(instance);
    return 1;
}

constexpr draxul::plugin_support::AdapterAction kActions[] = {
    { "rezonality_reload", "Reload Rezonality Project" },
};

using Presentation = draxul::plugin_support::PresentationAdapter<kActions,
    &get_presentation_state, &dispatch_action>;

const DraxulPluginApiV2 kApi = draxul::plugin_support::make_plugin_api(
    { kPluginId, "Rezonality", "0.1.0",
        draxul::plugin_support::kNativeBackendMask },
    {
        .create_instance = &create_instance,
        .quiesce_instance = &quiesce_instance,
        .destroy_instance = &destroy_instance,
        .set_viewport = &set_viewport,
        .set_visible = &set_visible,
        .set_focused = &set_focused,
        .handle_input = &handle_input,
        .tick = &tick,
#if defined(__APPLE__)
        .render_metal = &render_metal,
#else
        .render_vulkan = &render_vulkan,
#endif
        .query_extension = &Presentation::query_extension,
    });

} // namespace

extern "C" DRAXUL_PLUGIN_EXPORT const DraxulPluginApiV2*
draxul_plugin_query_v2(uint32_t requested_abi)
{
    return requested_abi == DRAXUL_PLUGIN_ABI_VERSION ? &kApi : nullptr;
}

