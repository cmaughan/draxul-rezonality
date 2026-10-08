#pragma once

#include "model_loader.h"
#include "project_file_index.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace rezonality
{

struct DecodedAssetCounters
{
    uint64_t model_decodes = 0;
    uint64_t model_reuses = 0;
    uint64_t image_decodes = 0;
    uint64_t image_reuses = 0;
};

// Immutable decoded models and images keyed by source path and decode
// options. An entry is reused only while every file the decode read or
// probed (for a model: buffers, material libraries, and textures) keeps the
// stat stamp it had when decoded. Entries whose inputs were still racy at
// decode time are not retained, so a rewrite inside the filesystem's
// timestamp granularity cannot hide behind an old stamp.
//
// Models are shared immutably with the candidates that use them, so a reused
// import costs no copy and no extra memory while a build still holds it.
// Image pixels are copied into each candidate (surfaces own plain vectors);
// at most kMaximumImageBytes of them are retained. Entries are bounded to the
// assets of the latest fully resolved scene plus the build in progress.
//
// Not thread-safe: one project worker owns each cache.
class DecodedAssetCache
{
public:
    static constexpr size_t kMaximumImageBytes = size_t{ 256 } * 1024 * 1024;

    bool load_model(const std::filesystem::path& path, const glm::vec3& scale,
        bool flip_texture_y, SharedModel& model, std::string& error);
    bool load_rgba8_image(const std::filesystem::path& path,
        uint32_t& width, uint32_t& height, std::vector<uint8_t>& pixels,
        std::string& error);
    bool load_rgba32f_image(const std::filesystem::path& path,
        uint32_t& width, uint32_t& height, std::vector<float>& pixels,
        std::string& error);

    // Ends one candidate's asset resolution. When every asset resolved, the
    // cache keeps only what that scene used; otherwise it also keeps the
    // previous complete scene's assets so a repaired build can reuse them.
    void finish_build(bool all_assets_resolved);

    [[nodiscard]] DecodedAssetCounters counters() const;
    [[nodiscard]] size_t cached_assets() const;
    [[nodiscard]] size_t cached_bytes() const;

private:
    struct Image8
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;
    };
    struct Image32
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<float> pixels;
    };
    struct Dependency
    {
        std::filesystem::path path;
        FileStamp stamp;
    };
    struct Entry
    {
        std::variant<SharedModel, Image8, Image32> value;
        std::vector<Dependency> dependencies;
        // Copied image payload counted against kMaximumImageBytes. Shared
        // models count zero: their storage belongs to the builds using them.
        size_t bytes = 0;
    };

    const Entry* find_current(const std::string& key);
    void store(const std::string& key, Entry entry,
        const std::vector<std::filesystem::path>& inputs,
        std::filesystem::file_time_type decode_started);
    void erase(const std::string& key);

    std::map<std::string, Entry> entries_;
    std::set<std::string> used_;
    std::set<std::string> last_complete_;
    size_t bytes_ = 0;
    std::atomic<uint64_t> model_decodes_{ 0 };
    std::atomic<uint64_t> model_reuses_{ 0 };
    std::atomic<uint64_t> image_decodes_{ 0 };
    std::atomic<uint64_t> image_reuses_{ 0 };
    std::atomic<size_t> cached_assets_{ 0 };
    std::atomic<size_t> cached_bytes_{ 0 };
};

} // namespace rezonality
