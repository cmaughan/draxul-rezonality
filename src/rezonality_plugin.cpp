#include "live_project.h"

#include "audio_analysis.h"
#include "animation_clock.h"
#include "camera.h"
#include "diagnostics.h"
#include "native_backend.h"
#include "runtime_controller.h"

#include <draxul/plugin_adapter.h>
#include <draxul/plugin_api.h>
#include <draxul/plugin_host_services.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using draxul::plugin_support::render_result;
using draxul::plugin_support::tick_result;
using rezonality::AudioAnalyzer;
using rezonality::AudioOptions;
using rezonality::AudioTextureFrame;
using rezonality::BuildResult;
using rezonality::DiagnosticEntry;
using rezonality::DiagnosticState;
using rezonality::DiagnosticsPublisher;
using rezonality::LiveProject;
using rezonality::ProjectOptions;
using rezonality::ShaderBuild;

constexpr const char* kPluginId = "dev.draxul.rezonality";
constexpr const char* kPluginVersion = "0.7.0";
struct RezonalityInstance
{
    const DraxulPluginHostApiV2* host = nullptr;
    std::filesystem::path plugin_directory;
    ProjectOptions options;
    std::unique_ptr<draxul::plugin_support::HostServices> services;
    DiagnosticsPublisher diagnostics;
    DraxulPluginViewportV2 viewport{};
    std::unique_ptr<LiveProject> project;
    AudioOptions audio_options;
    std::unique_ptr<AudioAnalyzer> audio;
    rezonality::RuntimeController runtime;
    rezonality::NativeBackend backend;
    bool focused = false;
    rezonality::AnimationClock animation;
    rezonality::Camera camera;
    bool camera_initialized = false;
    std::string presentation_status;
    std::string audio_status;
};

bool uses_audio(const ShaderBuild& build)
{
    return std::any_of(build.surfaces.begin(), build.surfaces.end(),
        [](const auto& surface) { return surface.audio_analysis; });
}

void configure_audio(RezonalityInstance* instance, const ShaderBuild& build)
{
    if (uses_audio(build))
    {
        if (!instance->audio)
        {
            instance->audio
                = std::make_unique<AudioAnalyzer>(instance->audio_options);
            instance->audio->set_visible(instance->runtime.visible());
        }
    }
    else
    {
        instance->audio.reset();
        instance->audio_status.clear();
    }
}

void ensure_camera(RezonalityInstance* instance, const ShaderBuild& build)
{
    if (!instance || instance->camera_initialized)
        return;
    const auto model_pass = std::find_if(build.passes.begin(),
        build.passes.end(), [](const auto& pass) {
            return pass.model_index.has_value();
        });
    if (model_pass != build.passes.end())
        instance->camera = model_pass->camera;
    else if (!build.passes.empty())
        instance->camera = build.passes.front().camera;
    instance->camera_initialized = true;
}

void log(RezonalityInstance* instance, uint32_t level,
    const std::string& message)
{
    if (instance && instance->host && instance->host->log)
        instance->host->log(instance->host->host_context,
            level, message.data(), message.size());
}

std::string diagnostic_stage(const BuildResult& result)
{
    const std::string extension = result.diagnostic_path.extension().string();
    if (extension == ".scenegraph" || extension == ".toml")
        return "parse";
    if (extension == ".vert" || extension == ".frag"
        || extension == ".geom" || extension == ".rgen"
        || extension == ".rmiss" || extension == ".rchit"
        || extension == ".metal")
        return "compile";
    return "prepare";
}

void publish_diagnostics(RezonalityInstance* instance,
    std::string stage, std::string severity,
    const std::filesystem::path& path, int line,
    std::string message,
    const std::vector<DiagnosticEntry>* diagnostics = nullptr)
{
    if (!instance || !instance->diagnostics.available())
        return;
    DiagnosticState state;
    state.project_path = instance->options.project_path;
    state.scenegraph_path
        = instance->options.project_path / instance->options.scenegraph;
    if (const auto& active = instance->runtime.active_build())
        state.active_source_files = active->source_files;
    if (const auto& pending = instance->runtime.pending_build())
        state.candidate_source_files = pending->source_files;
    state.path = path;
    state.attempted_generation = instance->runtime.attempted_generation();
    state.active_generation = instance->runtime.active_generation();
    state.last_success_unix_ms = instance->runtime.last_success_unix_ms();
    state.stage = std::move(stage);
    state.severity = std::move(severity);
    state.line = line;
    state.message = std::move(message);
    if (diagnostics && !diagnostics->empty())
    {
        state.diagnostics = *diagnostics;
    }
    else
    {
        state.diagnostics.push_back({
            .path = state.path,
            .stage = state.stage,
            .severity = state.severity,
            .line = state.line,
            .column = state.column,
            .message = state.message,
        });
    }
    std::string error;
    if (!instance->diagnostics.publish(state, error))
        log(instance, DRAXUL_PLUGIN_LOG_WARNING, error);
}

void request_tick(RezonalityInstance* instance)
{
    if (instance && instance->host && instance->host->request_tick)
        instance->host->request_tick(instance->host->host_context);
}

void request_redraw(RezonalityInstance* instance)
{
    if (instance && instance->host && instance->host->request_redraw)
        instance->host->request_redraw(instance->host->host_context);
}

void notify_presentation(RezonalityInstance* instance)
{
    if (instance && instance->host
        && instance->host->notify_presentation_changed)
        instance->host->notify_presentation_changed(
            instance->host->host_context);
}

void* create_instance(const DraxulPluginCreateInfoV2* info)
{
    if (!info || info->struct_size < sizeof(DraxulPluginCreateInfoV2)
        || !info->host
        || info->host->struct_size < sizeof(DraxulPluginHostApiV2)
        || info->host->abi_version != DRAXUL_PLUGIN_ABI_VERSION)
        return nullptr;

    auto instance = std::make_unique<RezonalityInstance>();
    instance->host = info->host;
    instance->plugin_directory = info->plugin_directory_utf8
        ? std::filesystem::u8path(info->plugin_directory_utf8)
        : std::filesystem::path{};
    instance->viewport = info->initial_viewport;
    std::string error;
    auto options = rezonality::parse_project_options(
        instance->plugin_directory, info->config_json,
        info->config_json_length, error);
    if (!options)
    {
        log(instance.get(), DRAXUL_PLUGIN_LOG_ERROR, error);
        return nullptr;
    }
    instance->options = *options;
    instance->services
        = std::make_unique<draxul::plugin_support::HostServices>(*info);
    instance->diagnostics = DiagnosticsPublisher(
        instance->services->path(DRAXUL_PLUGIN_PATH_CACHE),
        options->project_path, options->diagnostics_id);
    auto* raw = instance.get();
    instance->project = std::make_unique<LiveProject>(
        instance->plugin_directory, *options, [raw] {
            request_tick(raw);
        });
    instance->audio_options = options->audio;
    instance->animation.set_paused(instance->project->options().paused);
    instance->project->start();
    publish_diagnostics(instance.get(), "watch", "info", {}, -1,
        "building generation 1");
    return instance.release();
}

void quiesce_instance(void* opaque)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || instance->runtime.quiesced())
        return;
    instance->runtime.set_quiesced(true);
    if (instance->project)
        instance->project->stop();
}

void destroy_instance(void* opaque)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance)
        return;
    if (instance->project)
        instance->project->stop();
    std::string diagnostics_error;
    if (!instance->diagnostics.remove(diagnostics_error))
        log(instance, DRAXUL_PLUGIN_LOG_WARNING, diagnostics_error);
    delete instance;
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
    if (!instance)
        return;
    instance->runtime.set_visible(visible != 0);
    if (instance->audio)
        instance->audio->set_visible(instance->runtime.visible());
    if (instance->runtime.visible())
        request_redraw(instance);
    notify_presentation(instance);
}

void set_focused(void* opaque, int32_t focused)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance)
        return;
    instance->focused = focused != 0;
    notify_presentation(instance);
}

int32_t handle_input(void* opaque, const DraxulPluginInputEventV2* event)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !event
        || event->struct_size < sizeof(DraxulPluginInputEventV2))
        return 0;

    if (event->kind == DRAXUL_PLUGIN_INPUT_KEY
        && event->pressed && event->logical_key == 32)
    {
        instance->animation.set_paused(!instance->animation.paused);
        if (!instance->animation.paused)
            request_redraw(instance);
        notify_presentation(instance);
        return 1;
    }
    if (event->kind == DRAXUL_PLUGIN_INPUT_POINTER_MOVE
        && (event->buttons & 1u) != 0)
    {
        rezonality::camera_orbit(instance->camera,
            { event->delta_x * 0.25f, event->delta_y * 0.25f });
        request_redraw(instance);
        return 1;
    }
    if (event->kind == DRAXUL_PLUGIN_INPUT_WHEEL
        && event->delta_y != 0.0f)
    {
        rezonality::camera_dolly(instance->camera,
            event->delta_y * 0.35f);
        request_redraw(instance);
        return 1;
    }
    return event->kind == DRAXUL_PLUGIN_INPUT_POINTER_BUTTON
            && event->button == 1
        ? 1
        : 0;
}

DraxulPluginTickResultV2 tick(void* opaque,
    const DraxulPluginTickInfoV2*)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || instance->runtime.quiesced())
        return tick_result(false, DRAXUL_PLUGIN_NO_DEADLINE);
    if (auto result = instance->project->take_result())
    {
        const bool ready = result->build.has_value();
        instance->runtime.accept(*result);
        if (ready)
        {
            publish_diagnostics(instance, "build", "info", {}, -1,
                "candidate generation ready");
            if (instance->runtime.visible())
                request_redraw(instance);
        }
        else
        {
            log(instance, DRAXUL_PLUGIN_LOG_ERROR,
                instance->runtime.status());
            publish_diagnostics(instance, diagnostic_stage(*result),
                "error", result->diagnostic_path,
                result->diagnostic_line, result->error,
                &result->diagnostics);
        }
        notify_presentation(instance);
    }
    return tick_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
}

class BoundRuntimeBackend final : public rezonality::RuntimeBackend
{
public:
    explicit BoundRuntimeBackend(RezonalityInstance& instance)
        : instance_(instance)
    {
    }

    bool active_compatible() const override
    {
        return instance_.backend.active_compatible();
    }

    rezonality::BackendPreparation prepare(
        const ShaderBuild& build) override
    {
        ensure_camera(&instance_, build);
        return instance_.backend.prepare(build);
    }

    void activate_prepared() override
    {
        instance_.backend.activate_prepared();
    }

    void retire_completed_slot(uint32_t frame_index) override
    {
        instance_.backend.retire_completed_slot(frame_index);
    }

private:
    RezonalityInstance& instance_;
};

void report_preparation(RezonalityInstance* instance,
    const rezonality::RuntimePrepareResult& prepared)
{
    if (prepared.disposition
        == rezonality::RuntimePrepareDisposition::Activated)
    {
        configure_audio(instance, *instance->runtime.active_build());
        // A newer build can fail while an older, already-published candidate
        // is still waiting for its first render-thread activation. Keep that
        // newer failure authoritative until an equally new successful
        // generation activates.
        if (prepared.activated_latest_attempt())
        {
            publish_diagnostics(instance, "render", "info", {}, -1,
                "active generation ready");
        }
        notify_presentation(instance);
    }
    else if (prepared.disposition
        == rezonality::RuntimePrepareDisposition::Rejected)
    {
        log(instance, DRAXUL_PLUGIN_LOG_ERROR,
            instance->runtime.status());
        publish_diagnostics(instance, "prepare", "error", {}, -1,
            prepared.error);
        notify_presentation(instance);
    }
}

#if defined(__APPLE__)

DraxulPluginRenderResultV2 render_metal(void* opaque,
    const DraxulPluginMetalFrameV2* frame)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !instance->runtime.should_render())
        return render_result(instance != nullptr,
            DRAXUL_PLUGIN_NO_DEADLINE);
    if (!frame || frame->struct_size < sizeof(*frame)
        || !frame->device || !frame->command_buffer
        || !frame->drawable_texture
        || !frame->continuation_render_pass_descriptor)
        return render_result(false, DRAXUL_PLUGIN_NO_DEADLINE,
            "Rezonality received an incomplete Metal frame");
    if (frame->viewport.width <= 0 || frame->viewport.height <= 0)
        return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);

    const double animation_seconds = instance->animation.advance(
        frame->monotonic_seconds);
    instance->backend.bind_frame(
        *frame, animation_seconds, instance->camera);
    BoundRuntimeBackend backend(*instance);
    report_preparation(instance,
        instance->runtime.prepare_frame(backend, frame->frame_index));

    std::optional<AudioTextureFrame> audio;
    if (instance->audio)
    {
        audio = instance->audio->frame();
        instance->audio_status = audio->status;
    }
    return instance->backend.record(
        *frame, audio ? &*audio : nullptr, instance->animation.paused);
}

#else

DraxulPluginRenderResultV2 render_vulkan(void* opaque,
    const DraxulPluginVulkanFrameV2* frame)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !instance->runtime.should_render())
        return render_result(instance != nullptr,
            DRAXUL_PLUGIN_NO_DEADLINE);
    if (!frame || frame->struct_size < sizeof(*frame)
        || !frame->device || !frame->physical_device
        || !frame->command_buffer || !frame->continuation_render_pass
        || !frame->continuation_framebuffer)
        return render_result(false, DRAXUL_PLUGIN_NO_DEADLINE,
            "Rezonality received an incomplete Vulkan frame");
    if (frame->viewport.width <= 0 || frame->viewport.height <= 0)
        return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);

    const double animation_seconds = instance->animation.advance(
        frame->monotonic_seconds);
    instance->backend.bind_frame(
        *frame, animation_seconds, instance->camera);
    BoundRuntimeBackend backend(*instance);
    report_preparation(instance,
        instance->runtime.prepare_frame(backend, frame->frame_index));

    std::optional<AudioTextureFrame> audio;
    if (instance->audio)
    {
        audio = instance->audio->frame();
        instance->audio_status = audio->status;
    }
    return instance->backend.record(
        *frame, audio ? &*audio : nullptr, instance->animation.paused);
}

#endif

int32_t get_presentation_state(void* opaque,
    DraxulPluginPresentationStateV2* state)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !state
        || state->struct_size < sizeof(DraxulPluginPresentationStateV2))
        return 0;
    instance->presentation_status
        = instance->options.project_path.filename().string()
        + " | " + instance->runtime.status();
    if (!instance->audio_status.empty())
        instance->presentation_status += " | " + instance->audio_status;
    if (instance->animation.paused)
        instance->presentation_status += " | paused";
    if (!instance->runtime.visible())
        instance->presentation_status += " | hidden";
    if (instance->focused)
        instance->presentation_status += " | focused";

    *state = {};
    state->struct_size = sizeof(*state);
    state->display_name = { "Rezonality", 10 };
    state->status_text = { instance->presentation_status.data(),
        instance->presentation_status.size() };
    state->background_red = 0.025f;
    state->background_green = 0.035f;
    state->background_blue = 0.055f;
    state->background_alpha = 1.0f;
    state->content_ready = instance->runtime.quiesced() ? 0 : 1;
    state->mouse_cursor = DRAXUL_PLUGIN_CURSOR_DEFAULT;
    return 1;
}

int32_t dispatch_action(void* opaque, const char* action,
    size_t action_length)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !action || instance->runtime.quiesced()
        || std::string_view(action, action_length) != "rezonality_reload")
        return 0;
    instance->runtime.begin_reload();
    instance->project->force_reload();
    notify_presentation(instance);
    return 1;
}

constexpr draxul::plugin_support::AdapterAction kActions[] = {
    { "rezonality_reload", "Reload Rezonality Project" },
};

using Presentation = draxul::plugin_support::PresentationAdapter<kActions,
    &get_presentation_state, &dispatch_action>;

int32_t export_reload_json(void* opaque, char* buffer,
    size_t* in_out_size)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !in_out_size)
        return 0;
    const std::string value = nlohmann::json{
        { "project_path", instance->options.project_path.generic_string() },
        { "scenegraph", instance->options.scenegraph.generic_string() },
        { "time_seconds", instance->animation.elapsed_seconds },
        { "paused", instance->animation.paused },
        { "camera_position", {
            instance->camera.position.x,
            instance->camera.position.y,
            instance->camera.position.z } },
        { "camera_focal_point", {
            instance->camera.focal_point.x,
            instance->camera.focal_point.y,
            instance->camera.focal_point.z } },
    }.dump();
    const size_t required = value.size() + 1;
    if (!buffer)
    {
        *in_out_size = required;
        return required <= DRAXUL_PLUGIN_MAX_HOT_RELOAD_JSON_BYTES + 1;
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

std::optional<glm::vec3> reload_vec3(
    const nlohmann::json& state, const char* key)
{
    const auto value = state.find(key);
    if (value == state.end() || !value->is_array() || value->size() != 3)
        return std::nullopt;
    glm::vec3 result;
    for (size_t index = 0; index < 3; ++index)
    {
        if (!(*value)[index].is_number())
            return std::nullopt;
        result[index] = (*value)[index].get<float>();
        if (!std::isfinite(result[index]) || std::abs(result[index]) > 1e6f)
            return std::nullopt;
    }
    return result;
}

int32_t import_reload_json(void* opaque, const char* json,
    size_t json_length, const char* source_schema_id,
    uint32_t source_schema_version)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !json || !source_schema_id
        || std::string_view(source_schema_id)
            != "dev.draxul.rezonality.state"
        || source_schema_version != 1
        || json_length > DRAXUL_PLUGIN_MAX_HOT_RELOAD_JSON_BYTES)
        return 0;
    try
    {
        const auto state = nlohmann::json::parse(json, json + json_length);
        if (!state.is_object()
            || state.value("project_path", std::string{})
                != instance->options.project_path.generic_string()
            || state.value("scenegraph", std::string{})
                != instance->options.scenegraph.generic_string())
            return 0;
        const double time = state.value("time_seconds", 0.0);
        const auto position = reload_vec3(state, "camera_position");
        const auto focal_point = reload_vec3(state, "camera_focal_point");
        if (!std::isfinite(time) || time < 0.0 || time > 1e12
            || !position || !focal_point)
            return 0;
        instance->animation.elapsed_seconds = time;
        instance->animation.last_seconds = -1.0;
        instance->animation.set_paused(state.value("paused",
            instance->animation.paused));
        rezonality::camera_set_pos_lookat(
            instance->camera, *position, *focal_point);
        instance->camera_initialized = true;
        request_redraw(instance);
        notify_presentation(instance);
        return 1;
    }
    catch (...)
    {
        return 0;
    }
}

int32_t query_extension(void* instance, const char* extension_id,
    size_t extension_id_length, uint32_t requested_version,
    void* extension_table, size_t extension_table_size)
{
    const std::string_view id(extension_id ? extension_id : "",
        extension_id ? extension_id_length : 0);
    if (id == DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_ID
        && requested_version == DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_VERSION
        && extension_table
        && extension_table_size >= sizeof(DraxulPluginHotReloadExtensionV2))
    {
        auto* extension = static_cast<DraxulPluginHotReloadExtensionV2*>(
            extension_table);
        *extension = {
            sizeof(*extension), DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_VERSION,
            "dev.draxul.rezonality.state", 1,
            &export_reload_json, &import_reload_json
        };
        return 1;
    }
    return Presentation::query_extension(instance, extension_id,
        extension_id_length, requested_version, extension_table,
        extension_table_size);
}

const DraxulPluginApiV2 kApi = draxul::plugin_support::make_plugin_api(
    { kPluginId, "Rezonality", kPluginVersion,
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
        .query_extension = &query_extension,
    });

} // namespace

extern "C" DRAXUL_PLUGIN_EXPORT const DraxulPluginApiV2*
draxul_plugin_query_v2(uint32_t requested_abi)
{
    return requested_abi == DRAXUL_PLUGIN_ABI_VERSION ? &kApi : nullptr;
}
