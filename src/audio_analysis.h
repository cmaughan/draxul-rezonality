#pragma once

#include "audio_types.h"

#include <memory>

namespace rezonality
{

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
