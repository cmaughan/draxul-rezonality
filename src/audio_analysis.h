#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rezonality
{

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

// One analyzer belongs to each audio project instance. Live-input analyzers
// share one capture stream per selected device, while their plugin lifecycle,
// visibility and presentation state remain independent.
class AudioAnalyzer
{
public:
    explicit AudioAnalyzer(AudioOptions options);
    ~AudioAnalyzer();

    AudioAnalyzer(const AudioAnalyzer&) = delete;
    AudioAnalyzer& operator=(const AudioAnalyzer&) = delete;

    void set_visible(bool visible);
    AudioTextureFrame frame();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rezonality
