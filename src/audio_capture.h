#pragma once

#include "audio_types.h"
#include "mic_permission.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace rezonality::detail
{

using AudioDeviceHandle = uintptr_t;
using AudioStreamHandle = void*;

struct AudioInputDevice
{
    AudioDeviceHandle handle = 0;
    std::string name;
};

// Product-private native-operation boundary. Production callbacks delegate to
// SDL and AVFoundation; focused tests supply deterministic in-memory fakes.
struct AudioCaptureOperations
{
    AudioDeviceHandle default_recording_device = 0;
    std::function<bool()> initialize;
    std::function<MicPermission()> permission;
    std::function<void(std::stop_token, std::chrono::milliseconds)> wait;
    std::function<std::vector<AudioInputDevice>()> recording_devices;
    std::function<AudioStreamHandle(AudioDeviceHandle, std::stop_token)> open;
    std::function<bool(AudioStreamHandle)> resume;
    std::function<void(AudioStreamHandle)> pause;
    std::function<void(AudioStreamHandle)> clear;
    std::function<int(AudioStreamHandle)> available;
    std::function<int(AudioStreamHandle, float*, int)> read;
    std::function<void(AudioStreamHandle)> destroy;
    std::function<std::string()> last_error;
};

inline constexpr int max_buffered_audio_bytes
    = 48000 * 2 * static_cast<int>(sizeof(float)) * 3;

class AudioCaptureRegistry;

class AudioCaptureService final
{
public:
    ~AudioCaptureService();

    AudioCaptureService(const AudioCaptureService&) = delete;
    AudioCaptureService& operator=(const AudioCaptureService&) = delete;

    void add_visible();
    void remove_visible();
    AudioTextureFrame frame();

private:
    friend class AudioCaptureRegistry;
    AudioCaptureService(std::string device_name,
        std::shared_ptr<const AudioCaptureOperations> operations);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Production and each focused test own an explicit registry. Services share
// only within that registry and only for the exact requested device string.
class AudioCaptureRegistry final
{
public:
    explicit AudioCaptureRegistry(AudioCaptureOperations operations);
    ~AudioCaptureRegistry();

    AudioCaptureRegistry(const AudioCaptureRegistry&) = delete;
    AudioCaptureRegistry& operator=(const AudioCaptureRegistry&) = delete;

    std::shared_ptr<AudioCaptureService> capture(
        const std::string& device_name);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rezonality::detail
