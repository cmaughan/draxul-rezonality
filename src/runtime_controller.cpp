#include "runtime_controller.h"

#include <chrono>
#include <utility>

namespace rezonality
{
namespace
{

uint64_t unix_milliseconds()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

RuntimeTransition RuntimeController::accept(BuildResult& result)
{
    attempted_generation_ = result.generation;
    if (result.build)
    {
        pending_build_ = std::move(*result.build);
        status_ = "ready g" + std::to_string(result.generation);
    }
    else
    {
        status_ = failure_status(result.generation, active_generation_,
            result.diagnostic_path, result.diagnostic_line, result.error,
            result.diagnostics.empty() ? 0 : result.diagnostics.size() - 1);
    }
    return transition();
}

const ShaderBuild* RuntimeController::desired(
    bool active_backend_compatible) const
{
    if (pending_build_)
        return &*pending_build_;
    if (active_build_ && !active_backend_compatible)
        return &*active_build_;
    return nullptr;
}

RuntimeTransition RuntimeController::activate_prepared()
{
    const ShaderBuild* selected = pending_build_ ? &*pending_build_
                                                  : active_build_ ? &*active_build_
                                                                  : nullptr;
    if (!selected)
        return transition();

    // Copy before mutating either optional. This also keeps resize recreation
    // valid when the selected value already belongs to active_build_.
    ShaderBuild activated = *selected;
    const size_t pass_count = activated.passes.size();
    const size_t surface_count = activated.surfaces.size();
    active_generation_ = activated.generation;
    active_build_ = std::move(activated);
    pending_build_.reset();
    last_success_unix_ms_ = unix_milliseconds();
    status_ = "live g" + std::to_string(active_generation_) + " | "
        + std::to_string(pass_count) + " passes | "
        + std::to_string(surface_count) + " surfaces";

    RuntimeTransition result = transition();
    result.pass_count = pass_count;
    result.surface_count = surface_count;
    return result;
}

RuntimeTransition RuntimeController::reject_prepared(std::string_view error)
{
    const ShaderBuild* selected = pending_build_ ? &*pending_build_
                                                  : active_build_ ? &*active_build_
                                                                  : nullptr;
    const uint64_t generation = selected ? selected->generation
                                         : attempted_generation_;
    pending_build_.reset();
    status_ = failure_status(generation, active_generation_, {}, -1, error);
    return transition();
}

RuntimePrepareResult RuntimeController::prepare_frame(
    RuntimeBackend& backend, uint32_t completed_frame_slot)
{
    backend.retire_completed_slot(completed_frame_slot);
    const ShaderBuild* selected = desired(backend.active_compatible());
    if (!selected)
        return { .transition = transition() };

    BackendPreparation prepared = backend.prepare(*selected);
    if (!prepared.ready)
    {
        RuntimePrepareResult result;
        result.disposition = RuntimePrepareDisposition::Rejected;
        result.error = std::move(prepared.error);
        result.transition = reject_prepared(result.error);
        return result;
    }

    backend.activate_prepared();
    return {
        .disposition = RuntimePrepareDisposition::Activated,
        .transition = activate_prepared(),
    };
}

void RuntimeController::begin_reload()
{
    status_ = "building g" + std::to_string(attempted_generation_ + 1);
}

void RuntimeController::set_visible(bool visible)
{
    visible_ = visible;
}

void RuntimeController::set_quiesced(bool quiesced)
{
    quiesced_ = quiesced;
}

const std::optional<ShaderBuild>& RuntimeController::active_build() const
{
    return active_build_;
}

const std::optional<ShaderBuild>& RuntimeController::pending_build() const
{
    return pending_build_;
}

uint64_t RuntimeController::attempted_generation() const
{
    return attempted_generation_;
}

uint64_t RuntimeController::active_generation() const
{
    return active_generation_;
}

uint64_t RuntimeController::last_success_unix_ms() const
{
    return last_success_unix_ms_;
}

const std::string& RuntimeController::status() const
{
    return status_;
}

bool RuntimeController::visible() const
{
    return visible_;
}

bool RuntimeController::quiesced() const
{
    return quiesced_;
}

bool RuntimeController::should_render() const
{
    return visible_ && !quiesced_;
}

std::string RuntimeController::failure_status(uint64_t attempted_generation,
    uint64_t active_generation,
    const std::filesystem::path& diagnostic_path,
    int diagnostic_line, std::string_view error,
    size_t additional_diagnostics)
{
    std::string status = "BUILD FAILED g"
        + std::to_string(attempted_generation);
    if (active_generation != 0)
        status += " | rendering last good g"
            + std::to_string(active_generation);
    if (!diagnostic_path.empty())
    {
        status += " | " + diagnostic_path.filename().string();
        if (diagnostic_line > 0)
            status += ":" + std::to_string(diagnostic_line);
    }
    if (!error.empty())
        status += " | " + std::string(error);
    if (additional_diagnostics > 0)
        status += " | +" + std::to_string(additional_diagnostics) + " more";
    return status;
}

RuntimeTransition RuntimeController::transition() const
{
    return {
        .attempted_generation = attempted_generation_,
        .active_generation = active_generation_,
        .status = status_,
    };
}

} // namespace rezonality
