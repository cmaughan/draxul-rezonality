#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rezonality
{

bool load_rgba8_image(const std::filesystem::path& path,
    uint32_t& width, uint32_t& height, std::vector<uint8_t>& pixels,
    std::string& error);
bool load_rgba8_image(const uint8_t* bytes, size_t byte_count,
    uint32_t& width, uint32_t& height, std::vector<uint8_t>& pixels,
    std::string& error);
bool load_rgba32f_image(const std::filesystem::path& path,
    uint32_t& width, uint32_t& height, std::vector<float>& pixels,
    std::string& error);

} // namespace rezonality
