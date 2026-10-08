#include "project_file_index.h"
#include "path_utf8.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace rezonality
{
namespace
{

namespace fs = std::filesystem;

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t hash_bytes(uint64_t hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

uint64_t hash_value(uint64_t hash, uint64_t value)
{
    return hash_bytes(hash, &value, sizeof(value));
}

// Streams one file through the hash. Returns nullopt when the file cannot be
// opened or read completely.
std::optional<uint64_t> hash_file(const fs::path& path, uint64_t& bytes_read)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    uint64_t hash = kFnvOffset;
    std::vector<char> buffer(64 * 1024);
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<size_t>(input.gcount());
        bytes_read += count;
        hash = hash_bytes(hash, buffer.data(), count);
    }
    if (!input.eof())
        return std::nullopt;
    return hash;
}

FileStamp stamp_entry(const fs::directory_entry& entry)
{
    std::error_code size_error;
    std::error_code time_error;
    FileStamp stamp;
    stamp.size = entry.file_size(size_error);
    stamp.modified = entry.last_write_time(time_error);
    stamp.exists = !size_error && !time_error;
    if (!stamp.exists)
        return {};
    return stamp;
}

} // namespace

FileStamp stamp_file(const fs::path& path)
{
    std::error_code error;
    const fs::directory_entry entry(path, error);
    if (error)
        return {};
    const bool regular = entry.is_regular_file(error);
    if (error || !regular)
        return {};
    return stamp_entry(entry);
}

bool stamp_is_racy(const FileStamp& stamp, fs::file_time_type observed_at)
{
    return stamp.exists && stamp.modified + kRacyStampWindow >= observed_at;
}

uint64_t ProjectFileIndex::fingerprint(const fs::path& root)
{
    // Captured before any stamp is read: a write that lands during this scan
    // has a modification time at or after this point and is therefore racy.
    const fs::file_time_type scan_started = fs::file_time_type::clock::now();
    ++scans_;

    std::error_code ec;
    fs::recursive_directory_iterator iterator(root,
        fs::directory_options::skip_permission_denied, ec);
    if (ec)
        throw std::runtime_error("could not scan Rezonality project: "
            + ec.message());
    std::vector<fs::path> files;
    for (; iterator != fs::recursive_directory_iterator(); iterator.increment(ec))
    {
        if (ec)
            throw std::runtime_error("could not scan Rezonality project: "
                + ec.message());
        const auto& entry = *iterator;
        // Shader includes have no required suffix. Ignore only repository
        // metadata, not unrecognized files that a shader may include.
        if (entry.is_directory() && entry.path().filename() == ".git")
        {
            iterator.disable_recursion_pending();
            continue;
        }
        std::error_code entry_error;
        if (entry.is_regular_file(entry_error))
            files.push_back(entry.path());
        else if (entry_error)
            throw std::runtime_error("could not inspect Rezonality project file: "
                + entry_error.message());
    }
    if (ec)
        throw std::runtime_error("could not scan Rezonality project: "
            + ec.message());
    std::sort(files.begin(), files.end());

    // Rebuilding the map from this scan keeps the index bounded by the
    // project's current file count; deleted files drop out immediately.
    std::map<fs::path, Entry> next;
    uint64_t hash = kFnvOffset;
    for (const auto& file : files)
    {
        ++files_seen_;
        const std::string path_text = generic_path_utf8(file);
        hash = hash_bytes(hash, path_text.data(), path_text.size());

        // Stat by path rather than trusting iteration-time directory data,
        // which Windows can report stale for a file still open for writing.
        const FileStamp stamp = stamp_file(file);
        Entry entry;
        const auto previous = entries_.find(file);
        if (stamp.exists && previous != entries_.end()
            && previous->second.trusted && previous->second.stamp == stamp)
        {
            entry = previous->second;
        }
        else if (stamp.exists)
        {
            uint64_t bytes = 0;
            const auto content = hash_file(file, bytes);
            ++files_hashed_;
            bytes_hashed_ += bytes;
            if (content)
            {
                entry.stamp = stamp;
                entry.content_hash = *content;
                // A racy or concurrently rewritten file is hashed again on
                // the next scan until its stamp is old enough to identify
                // its bytes.
                entry.trusted = !stamp_is_racy(stamp, scan_started)
                    && stamp_file(file) == stamp;
            }
        }
        // Unreadable or vanished files contribute only their path and are
        // retried on the next scan.
        if (entry.stamp.exists)
            hash = hash_value(hash, entry.content_hash);
        next.emplace(file, entry);
    }
    entries_ = std::move(next);
    indexed_files_ = entries_.size();
    return hash;
}

ProjectWatchCounters ProjectFileIndex::counters() const
{
    return {
        .scans = scans_.load(),
        .files_seen = files_seen_.load(),
        .files_hashed = files_hashed_.load(),
        .bytes_hashed = bytes_hashed_.load(),
    };
}

size_t ProjectFileIndex::indexed_files() const
{
    return indexed_files_.load();
}

} // namespace rezonality
