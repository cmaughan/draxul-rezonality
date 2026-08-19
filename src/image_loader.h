#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rezonality
{

bool load_rgba8_image(const std::filesystem::path& path,
    uint32_t& width, uint32_t& height, std::vector<uint8_t>& pixels,
    std::string& error);

} // namespace rezonality
