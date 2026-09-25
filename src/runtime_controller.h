#pragma once

#include "live_project.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace rezonality
{

struct RuntimeTransition
{
    uint64_t attempted_generation = 0;
    uint64_t active_generation = 0;
    size_t pass_count = 0;
    size_t surface_count = 0;
    std::string status;
};

struct BackendPreparation
{
    bool ready = false;
    std::string error;
};

class RuntimeBackend
{
public:
    virtual ~RuntimeBackend() = default;

    [[nodiscard]] virtual bool active_compatible() const = 0;
    virtual BackendPreparation prepare(const ShaderBuild& build) = 0;
    virtual void activate_prepared() = 0;
    virtual void retire_completed_slot(uint32_t frame_index) = 0;
};

enum class RuntimePrepareDisposition
{
    Unchanged,
    Activated,
    Rejected,
};

struct RuntimePrepareResult
{
    RuntimePrepareDisposition disposition
        = RuntimePrepareDisposition::Unchanged;
    RuntimeTransition transition;
    std::string error;

    [[nodiscard]] bool activated_latest_attempt() const
    {
        return disposition == RuntimePrepareDisposition::Activated
            && transition.active_generation
                == transition.attempted_generation;
    }
};

// Owns candidate selection and activation policy independently of Metal,
// Vulkan, the plugin ABI, and presentation callbacks. Native backends only
// prepare/record resources and report their typed success or failure here.
class RuntimeController
{
public:
    RuntimeTransition accept(BuildResult& result);

    [[nodiscard]] const ShaderBuild* desired(
        bool active_backend_compatible) const;
    RuntimeTransition activate_prepared();
    RuntimeTransition reject_prepared(std::string_view error);

    RuntimePrepareResult prepare_frame(
        RuntimeBackend& backend, uint32_t completed_frame_slot);

    void begin_reload();
    void set_visible(bool visible);
    void set_quiesced(bool quiesced);

    [[nodiscard]] const std::optional<ShaderBuild>& active_build() const;
    [[nodiscard]] const std::optional<ShaderBuild>& pending_build() const;
    [[nodiscard]] uint64_t attempted_generation() const;
    [[nodiscard]] uint64_t active_generation() const;
    [[nodiscard]] uint64_t last_success_unix_ms() const;
    [[nodiscard]] const std::string& status() const;
    [[nodiscard]] bool visible() const;
    [[nodiscard]] bool quiesced() const;
    [[nodiscard]] bool should_render() const;

private:
    static std::string failure_status(uint64_t attempted_generation,
        uint64_t active_generation,
        const std::filesystem::path& diagnostic_path,
        int diagnostic_line, std::string_view error,
        size_t additional_diagnostics = 0);
    RuntimeTransition transition() const;

    std::optional<ShaderBuild> pending_build_;
    std::optional<ShaderBuild> active_build_;
    uint64_t attempted_generation_ = 0;
    uint64_t active_generation_ = 0;
    uint64_t last_success_unix_ms_ = 0;
    std::string status_ = "building g1";
    bool visible_ = true;
    bool quiesced_ = false;
};

} // namespace rezonality
