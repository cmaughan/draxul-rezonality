#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rezonality
{

// These values cross the project scheduler, runtime controller, and capture
// implementation boundary. Keep them free of SDL and native-renderer headers.
struct AudioOptions
{
    enum class Source
    {
        Input,
        Synthetic,
        Silent,
    };

    Source source = Source::Input;
    std::string device_name;
};

struct AudioTextureFrame
{
    static constexpr uint32_t width = 512;
    static constexpr uint32_t height = 4;

    uint64_t generation = 0;
    std::vector<float> rgba;
    std::string status;
};

} // namespace rezonality
