#include "decoded_asset_cache.h"
#include "image_loader.h"
#include "path_utf8.h"

#include <bit>
#include <utility>

namespace rezonality
{
namespace
{

namespace fs = std::filesystem;

std::string model_key(const fs::path& path, const glm::vec3& scale,
    bool flip_texture_y)
{
    return "model\n" + generic_path_utf8(path) + "\n"
        + std::to_string(std::bit_cast<uint32_t>(scale.x)) + ","
        + std::to_string(std::bit_cast<uint32_t>(scale.y)) + ","
        + std::to_string(std::bit_cast<uint32_t>(scale.z))
        + (flip_texture_y ? ",flip" : ",keep");
}

std::string image_key(std::string_view kind, const fs::path& path)
{
    return std::string(kind) + "\n" + generic_path_utf8(path);
}

} // namespace

const DecodedAssetCache::Entry* DecodedAssetCache::find_current(
    const std::string& key)
{
    const auto found = entries_.find(key);
    if (found == entries_.end())
        return nullptr;
    for (const Dependency& dependency : found->second.dependencies)
    {
        if (stamp_file(dependency.path) != dependency.stamp)
        {
            erase(key);
            return nullptr;
        }
    }
    used_.insert(key);
    return &found->second;
}

void DecodedAssetCache::store(const std::string& key, Entry entry,
    const std::vector<fs::path>& inputs, fs::file_time_type decode_started)
{
    erase(key);
    used_.insert(key);
    std::set<fs::path> unique(inputs.begin(), inputs.end());
    entry.dependencies.reserve(unique.size());
    for (const fs::path& input : unique)
    {
        Dependency dependency{ input, stamp_file(input) };
        // Rewritten during or just before the decode: the stamp cannot prove
        // these bytes were the ones decoded, so do not retain the result.
        if (stamp_is_racy(dependency.stamp, decode_started))
            return;
        entry.dependencies.push_back(std::move(dependency));
    }
    if (entry.bytes > kMaximumImageBytes - bytes_)
        return;
    bytes_ += entry.bytes;
    entries_.insert_or_assign(key, std::move(entry));
    cached_assets_ = entries_.size();
    cached_bytes_ = bytes_;
}

void DecodedAssetCache::erase(const std::string& key)
{
    const auto found = entries_.find(key);
    if (found == entries_.end())
        return;
    bytes_ -= found->second.bytes;
    entries_.erase(found);
    cached_assets_ = entries_.size();
    cached_bytes_ = bytes_;
}

bool DecodedAssetCache::load_model(const fs::path& path,
    const glm::vec3& scale, bool flip_texture_y, SharedModel& model,
    std::string& error)
{
    const std::string key = model_key(path, scale, flip_texture_y);
    if (const Entry* cached = find_current(key))
    {
        model = std::get<SharedModel>(cached->value);
        ++model_reuses_;
        return true;
    }

    const fs::file_time_type decode_started
        = fs::file_time_type::clock::now();
    std::vector<fs::path> inputs;
    ModelData decoded;
    ++model_decodes_;
    if (!rezonality::load_model(
            path, scale, flip_texture_y, decoded, error, &inputs))
    {
        erase(key);
        return false;
    }
    model = SharedModel(std::move(decoded));
    Entry entry;
    entry.value = model;
    store(key, std::move(entry), inputs, decode_started);
    return true;
}

bool DecodedAssetCache::load_rgba8_image(const fs::path& path,
    uint32_t& width, uint32_t& height, std::vector<uint8_t>& pixels,
    std::string& error)
{
    const std::string key = image_key("rgba8", path);
    if (const Entry* cached = find_current(key))
    {
        const auto& image = std::get<Image8>(cached->value);
        width = image.width;
        height = image.height;
        pixels = image.pixels;
        ++image_reuses_;
        return true;
    }

    const fs::file_time_type decode_started
        = fs::file_time_type::clock::now();
    ++image_decodes_;
    if (!rezonality::load_rgba8_image(path, width, height, pixels, error))
    {
        erase(key);
        return false;
    }
    Entry entry;
    entry.bytes = pixels.size();
    entry.value = Image8{ width, height, pixels };
    store(key, std::move(entry), { path }, decode_started);
    return true;
}

bool DecodedAssetCache::load_rgba32f_image(const fs::path& path,
    uint32_t& width, uint32_t& height, std::vector<float>& pixels,
    std::string& error)
{
    const std::string key = image_key("rgba32f", path);
    if (const Entry* cached = find_current(key))
    {
        const auto& image = std::get<Image32>(cached->value);
        width = image.width;
        height = image.height;
        pixels = image.pixels;
        ++image_reuses_;
        return true;
    }

    const fs::file_time_type decode_started
        = fs::file_time_type::clock::now();
    ++image_decodes_;
    if (!rezonality::load_rgba32f_image(path, width, height, pixels, error))
    {
        erase(key);
        return false;
    }
    Entry entry;
    entry.bytes = pixels.size() * sizeof(float);
    entry.value = Image32{ width, height, pixels };
    store(key, std::move(entry), { path }, decode_started);
    return true;
}

void DecodedAssetCache::finish_build(bool all_assets_resolved)
{
    if (all_assets_resolved)
        last_complete_ = used_;
    for (auto entry = entries_.begin(); entry != entries_.end();)
    {
        if (used_.contains(entry->first)
            || last_complete_.contains(entry->first))
        {
            ++entry;
            continue;
        }
        bytes_ -= entry->second.bytes;
        entry = entries_.erase(entry);
    }
    used_.clear();
    cached_assets_ = entries_.size();
    cached_bytes_ = bytes_;
}

DecodedAssetCounters DecodedAssetCache::counters() const
{
    return {
        .model_decodes = model_decodes_.load(),
        .model_reuses = model_reuses_.load(),
        .image_decodes = image_decodes_.load(),
        .image_reuses = image_reuses_.load(),
    };
}

size_t DecodedAssetCache::cached_assets() const
{
    return cached_assets_.load();
}

size_t DecodedAssetCache::cached_bytes() const
{
    return cached_bytes_.load();
}

} // namespace rezonality
