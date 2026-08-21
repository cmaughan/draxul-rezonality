#include "diagnostics.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace rezonality
{
namespace
{

constexpr size_t kMaximumMessageBytes = 16u * 1024u;
constexpr size_t kMaximumPathBytes = 4u * 1024u;
constexpr size_t kMaximumDiagnostics = 128;
constexpr size_t kMaximumSourceFiles = 1024;
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

std::string bounded(std::string value, size_t maximum)
{
    if (value.size() > maximum)
        value.resize(maximum);
    return value;
}

std::string path_string(const std::filesystem::path& path)
{
    return bounded(path.generic_string(), kMaximumPathBytes);
}

uint64_t unix_milliseconds()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string diagnostics_id(const std::filesystem::path& project,
    std::string configured)
{
    if (!configured.empty())
        return configured;
    std::string stem = project.filename().string();
    std::transform(stem.begin(), stem.end(), stem.begin(),
        [](unsigned char value) {
            if (std::isalnum(value))
                return static_cast<char>(std::tolower(value));
            return '-';
        });
    if (stem.empty())
        stem = "project";
    if (stem.size() > 40)
        stem.resize(40);

    uint64_t hash = kFnvOffset;
    const std::string key = project.generic_string();
    for (unsigned char value : key)
    {
        hash ^= value;
        hash *= kFnvPrime;
    }
    std::ostringstream suffix;
    suffix << std::hex << std::setfill('0') << std::setw(8)
           << static_cast<uint32_t>(hash);
    return stem + "-" + suffix.str();
}

bool replace_file(const std::filesystem::path& temporary,
    const std::filesystem::path& target, std::string& error)
{
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    error = "could not publish diagnostics (Windows error "
        + std::to_string(GetLastError()) + ")";
    return false;
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary, target, rename_error);
    if (!rename_error)
        return true;
    error = "could not publish diagnostics: " + rename_error.message();
    return false;
#endif
}

} // namespace

DiagnosticsPublisher::DiagnosticsPublisher(std::filesystem::path cache_root,
    const std::filesystem::path& project_path, std::string configured_id)
{
    if (!cache_root.empty())
    {
        path_ = std::move(cache_root) / "diagnostics"
            / (diagnostics_id(project_path, std::move(configured_id))
                + ".json");
    }
}

bool DiagnosticsPublisher::available() const
{
    return !path_.empty();
}

const std::filesystem::path& DiagnosticsPublisher::path() const
{
    return path_;
}

bool DiagnosticsPublisher::publish(
    const DiagnosticState& state, std::string& error) const
{
    if (path_.empty())
        return true;
    nlohmann::json entries = nlohmann::json::array();
    const size_t entry_count = std::min(
        state.diagnostics.size(), kMaximumDiagnostics);
    for (size_t index = 0; index < entry_count; ++index)
    {
        const auto& entry = state.diagnostics[index];
        entries.push_back({
            { "stage", bounded(entry.stage, 32) },
            { "severity", bounded(entry.severity, 16) },
            { "path", path_string(entry.path) },
            { "line", entry.line },
            { "column", entry.column },
            { "message", bounded(entry.message, kMaximumMessageBytes) },
        });
    }

    nlohmann::json source_files = nlohmann::json::array();
    const size_t source_file_count = std::min(
        state.active_source_files.size(), kMaximumSourceFiles);
    for (size_t index = 0; index < source_file_count; ++index)
        source_files.push_back(path_string(state.active_source_files[index]));
    nlohmann::json candidate_source_files = nlohmann::json::array();
    const size_t candidate_source_file_count = std::min(
        state.candidate_source_files.size(), kMaximumSourceFiles);
    for (size_t index = 0; index < candidate_source_file_count; ++index)
    {
        candidate_source_files.push_back(
            path_string(state.candidate_source_files[index]));
    }

    const nlohmann::json document{
        { "schema_version", 3 },
        { "plugin_id", "dev.draxul.rezonality" },
        { "project_path", path_string(state.project_path) },
        { "scenegraph_path", path_string(state.scenegraph_path) },
        { "active_source_file_count", state.active_source_files.size() },
        { "active_source_files_truncated",
            state.active_source_files.size() > source_file_count },
        { "active_source_files", std::move(source_files) },
        { "candidate_source_file_count",
            state.candidate_source_files.size() },
        { "candidate_source_files_truncated",
            state.candidate_source_files.size() > candidate_source_file_count },
        { "candidate_source_files", std::move(candidate_source_files) },
        { "attempted_generation", state.attempted_generation },
        { "active_generation", state.active_generation },
        { "timestamp_unix_ms", unix_milliseconds() },
        { "last_success_unix_ms", state.last_success_unix_ms },
        { "stage", bounded(state.stage, 32) },
        { "severity", bounded(state.severity, 16) },
        { "path", path_string(state.path) },
        { "line", state.line },
        { "column", state.column },
        { "message", bounded(state.message, kMaximumMessageBytes) },
        { "diagnostic_count", state.diagnostics.size() },
        { "diagnostics_truncated", state.diagnostics.size() > entry_count },
        { "diagnostics", std::move(entries) },
    };

    std::error_code directory_error;
    std::filesystem::create_directories(path_.parent_path(), directory_error);
    if (directory_error)
    {
        error = "could not create diagnostics directory: "
            + directory_error.message();
        return false;
    }
    const std::filesystem::path temporary = path_.string() + ".tmp";
    {
        std::ofstream output(temporary,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "could not write diagnostics temporary file";
            return false;
        }
        output << document.dump(2) << '\n';
        if (!output.good())
        {
            error = "could not finish diagnostics temporary file";
            return false;
        }
    }
    if (replace_file(temporary, path_, error))
        return true;
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    return false;
}

bool DiagnosticsPublisher::remove(std::string& error) const
{
    if (path_.empty())
        return true;
    std::error_code remove_error;
    std::filesystem::remove(path_, remove_error);
    if (!remove_error)
        return true;
    error = "could not remove diagnostics: " + remove_error.message();
    return false;
}

} // namespace rezonality
