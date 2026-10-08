#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>

namespace rezonality
{

// Cheap stat-only identity for one file. Two stamps that compare equal are
// treated as unchanged unless the stamp was "racy" when it was recorded.
struct FileStamp
{
    bool exists = false;
    uintmax_t size = 0;
    std::filesystem::file_time_type modified{};

    bool operator==(const FileStamp&) const = default;
};

// Filesystems record modification times with coarse granularity (for example
// one second on HFS+ and two on FAT). A file whose recorded time is within
// this window of the observation can still be rewritten without the stamp
// changing, so such a stamp is never trusted on its own.
inline constexpr std::chrono::seconds kRacyStampWindow{ 2 };

[[nodiscard]] FileStamp stamp_file(const std::filesystem::path& path);
[[nodiscard]] bool stamp_is_racy(const FileStamp& stamp,
    std::filesystem::file_time_type observed_at);

struct ProjectWatchCounters
{
    uint64_t scans = 0;
    uint64_t files_seen = 0;
    uint64_t files_hashed = 0;
    uint64_t bytes_hashed = 0;
};

// Content fingerprint of every regular file below a project root. The index
// remembers one stamp and content hash per file, so an unchanged project is
// fingerprinted by directory enumeration and stat alone. A file is reread
// only when its size or modification time changes, when it is new, when it
// could not be read before, or while its stamp is racy. The fingerprint still
// covers paths and contents, so touching a file without changing its bytes
// does not report a change.
class ProjectFileIndex
{
public:
    // Throws std::runtime_error when the project cannot be enumerated.
    [[nodiscard]] uint64_t fingerprint(const std::filesystem::path& root);

    [[nodiscard]] ProjectWatchCounters counters() const;
    [[nodiscard]] size_t indexed_files() const;

private:
    struct Entry
    {
        FileStamp stamp;
        uint64_t content_hash = 0;
        bool trusted = false;
    };

    std::map<std::filesystem::path, Entry> entries_;
    std::atomic<uint64_t> scans_{ 0 };
    std::atomic<uint64_t> files_seen_{ 0 };
    std::atomic<uint64_t> files_hashed_{ 0 };
    std::atomic<uint64_t> bytes_hashed_{ 0 };
    std::atomic<size_t> indexed_files_{ 0 };
};

} // namespace rezonality
