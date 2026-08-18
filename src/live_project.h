#pragma once

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
    uint32_t compile_debounce_ms = 150;
};

struct ShaderBuild
{
    uint64_t generation = 0;
    std::filesystem::path project_path;
    std::filesystem::path scenegraph_path;
    std::filesystem::path vertex_path;
    std::filesystem::path fragment_path;
    std::vector<uint32_t> vertex_spirv;
    std::vector<uint32_t> fragment_spirv;
};

struct BuildResult
{
    uint64_t generation = 0;
    std::optional<ShaderBuild> build;
    std::filesystem::path diagnostic_path;
    int diagnostic_line = -1;
    std::string error;
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

    const ProjectOptions& options() const { return options_; }

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
