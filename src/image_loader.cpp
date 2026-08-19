#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "image_loader.h"

namespace rezonality
{

bool load_rgba8_image(const std::filesystem::path& path,
    uint32_t& width, uint32_t& height, std::vector<uint8_t>& pixels,
    std::string& error)
{
    int loaded_width = 0;
    int loaded_height = 0;
    int channels = 0;
    stbi_uc* loaded = stbi_load(path.string().c_str(), &loaded_width,
        &loaded_height, &channels, STBI_rgb_alpha);
    if (!loaded || loaded_width <= 0 || loaded_height <= 0)
    {
        error = "Could not load texture '" + path.string() + "': "
            + (stbi_failure_reason() ? stbi_failure_reason() : "unknown error");
        if (loaded)
            stbi_image_free(loaded);
        return false;
    }
    width = static_cast<uint32_t>(loaded_width);
    height = static_cast<uint32_t>(loaded_height);
    pixels.assign(loaded,
        loaded + static_cast<size_t>(width) * height * STBI_rgb_alpha);
    stbi_image_free(loaded);
    return true;
}

bool load_rgba8_image(const uint8_t* bytes, size_t byte_count,
    uint32_t& width, uint32_t& height, std::vector<uint8_t>& pixels,
    std::string& error)
{
    int loaded_width = 0;
    int loaded_height = 0;
    int channels = 0;
    stbi_uc* loaded = stbi_load_from_memory(bytes,
        static_cast<int>(byte_count), &loaded_width, &loaded_height,
        &channels, STBI_rgb_alpha);
    if (!loaded || loaded_width <= 0 || loaded_height <= 0)
    {
        error = std::string("Could not load embedded texture: ")
            + (stbi_failure_reason() ? stbi_failure_reason() : "unknown error");
        if (loaded)
            stbi_image_free(loaded);
        return false;
    }
    width = static_cast<uint32_t>(loaded_width);
    height = static_cast<uint32_t>(loaded_height);
    pixels.assign(loaded,
        loaded + static_cast<size_t>(width) * height * STBI_rgb_alpha);
    stbi_image_free(loaded);
    return true;
}

bool load_rgba32f_image(const std::filesystem::path& path,
    uint32_t& width, uint32_t& height, std::vector<float>& pixels,
    std::string& error)
{
    int loaded_width = 0;
    int loaded_height = 0;
    int channels = 0;
    float* loaded = stbi_loadf(path.string().c_str(), &loaded_width,
        &loaded_height, &channels, STBI_rgb_alpha);
    if (!loaded || loaded_width <= 0 || loaded_height <= 0)
    {
        error = "Could not load HDR texture '" + path.string() + "': "
            + (stbi_failure_reason() ? stbi_failure_reason() : "unknown error");
        if (loaded)
            stbi_image_free(loaded);
        return false;
    }
    width = static_cast<uint32_t>(loaded_width);
    height = static_cast<uint32_t>(loaded_height);
    pixels.assign(loaded,
        loaded + static_cast<size_t>(width) * height * STBI_rgb_alpha);
    stbi_image_free(loaded);
    return true;
}

} // namespace rezonality
