#pragma once

#include "live_project.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace rezonality
{

struct SurfaceDimensions
{
    uint32_t width = 0;
    uint32_t height = 0;
};

inline bool checked_surface_dimensions(const ShaderBuild::Surface& source,
    uint32_t pane_width, uint32_t pane_height, uint32_t device_limit,
    SurfaceDimensions& dimensions, std::string& error)
{
    const auto dimension = [&](uint32_t image_size, uint32_t pane_size,
                               float scale, const char* axis,
                               uint32_t& result) {
        if (device_limit == 0 || !std::isfinite(scale))
        {
            error = "Rezonality surface '" + source.name
                + "' has an invalid " + axis + " scale or device limit";
            return false;
        }
        const double computed = image_size != 0
            ? static_cast<double>(image_size)
            : static_cast<double>(pane_size)
                * static_cast<double>(std::max(0.01f, scale));
        if (!std::isfinite(computed) || computed > device_limit
            || computed > std::numeric_limits<uint32_t>::max())
        {
            error = "Rezonality surface '" + source.name + "' " + axis
                + " exceeds the device 2D texture limit ("
                + std::to_string(device_limit) + ")";
            return false;
        }
        result = std::max(1u, static_cast<uint32_t>(computed));
        return true;
    };
    return dimension(source.image_width, pane_width, source.scale_x,
               "width", dimensions.width)
        && dimension(source.image_height, pane_height, source.scale_y,
            "height", dimensions.height);
}

} // namespace rezonality
