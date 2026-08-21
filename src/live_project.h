#pragma once

#include "audio_analysis.h"
#include "camera.h"
#include "diagnostics.h"
#include "model_loader.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rezonality
{

struct ProjectOptions
{
    std::filesystem::path project_path;
    std::filesystem::path scenegraph = "default.scenegraph";
    bool auto_reload = true;
    bool paused = false;
    uint32_t compile_debounce_ms = 150;
    std::string diagnostics_id;
    AudioOptions audio;
};

struct ShaderBuild
{
    enum class SurfaceFormat
    {
        Color8,
        Color16Float,
        Color32Float,
        Depth32,
    };

    struct Surface
    {
        std::string name;
        std::filesystem::path path;
        SurfaceFormat format = SurfaceFormat::Color8;
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        bool target = false;
        uint32_t image_width = 0;
        uint32_t image_height = 0;
        std::vector<uint8_t> image_pixels;
        std::vector<float> image_float_pixels;
        bool audio_analysis = false;
    };

    struct Sampler
    {
        std::string surface;
        bool previous_frame = false;
    };

    struct Pass
    {
        std::string name;
        bool ray_trace = false;
        std::filesystem::path vertex_path;
        std::filesystem::path fragment_path;
        std::filesystem::path raygen_path;
        std::filesystem::path miss_path;
        std::filesystem::path closest_hit_path;
        std::filesystem::path metal_ray_path;
        std::vector<uint32_t> vertex_spirv;
        std::vector<uint32_t> fragment_spirv;
        std::vector<uint32_t> raygen_spirv;
        std::vector<uint32_t> miss_spirv;
        std::vector<uint32_t> closest_hit_spirv;
        std::string metal_ray_source;
        std::vector<std::string> targets;
        std::vector<Sampler> samplers;
        std::optional<size_t> model_index;
        Camera camera;
        float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        bool has_clear = false;
    };

    uint64_t generation = 0;
    std::filesystem::path project_path;
    std::filesystem::path scenegraph_path;
    std::vector<std::filesystem::path> source_files;
    std::vector<Surface> surfaces;
    std::vector<ModelData> models;
    std::vector<Pass> passes;
};

struct BuildResult
{
    uint64_t generation = 0;
    std::optional<ShaderBuild> build;
    std::filesystem::path diagnostic_path;
    int diagnostic_line = -1;
    std::string error;
    std::vector<DiagnosticEntry> diagnostics;
};

class LiveProject
{
public:
    using WakeCallback = std::function<void()>;

    LiveProject(std::filesystem::path plugin_directory,
        ProjectOptions options, WakeCallback wake);
    ~LiveProject();

    LiveProject(const LiveProject&) = delete;
    LiveProject& operator=(const LiveProject&) = delete;

    void start();
    void stop();
    void force_reload();
    std::optional<BuildResult> take_result();

    const ProjectOptions& options() const
    {
        return options_;
    }

private:
    void run(std::stop_token stop_token);
    BuildResult build(uint64_t generation) const;
    uint64_t project_fingerprint() const;

    std::filesystem::path plugin_directory_;
    ProjectOptions options_;
    WakeCallback wake_;
    std::jthread worker_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_condition_;
    std::optional<BuildResult> result_;
    bool force_requested_ = false;
};

std::optional<ProjectOptions> parse_project_options(
    const std::filesystem::path& plugin_directory,
    const char* config_json, size_t config_json_length,
    std::string& error);

} // namespace rezonality
