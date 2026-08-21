#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rezonality
{

struct DiagnosticEntry
{
    std::filesystem::path path;
    std::string stage;
    std::string severity;
    int line = -1;
    int column = -1;
    std::string message;
};

struct DiagnosticState
{
    std::filesystem::path project_path;
    std::filesystem::path scenegraph_path;
    std::vector<std::filesystem::path> active_source_files;
    std::vector<std::filesystem::path> candidate_source_files;
    std::filesystem::path path;
    uint64_t attempted_generation = 0;
    uint64_t active_generation = 0;
    uint64_t last_success_unix_ms = 0;
    std::string stage;
    std::string severity;
    int line = -1;
    int column = -1;
    std::string message;
    std::vector<DiagnosticEntry> diagnostics;
};

class DiagnosticsPublisher
{
public:
    DiagnosticsPublisher() = default;
    DiagnosticsPublisher(std::filesystem::path cache_root,
        const std::filesystem::path& project_path,
        std::string configured_id);

    [[nodiscard]] bool available() const;
    [[nodiscard]] const std::filesystem::path& path() const;
    bool publish(const DiagnosticState& state, std::string& error) const;
    bool remove(std::string& error) const;

private:
    std::filesystem::path path_;
};

} // namespace rezonality
