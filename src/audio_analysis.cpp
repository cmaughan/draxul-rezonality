#include "audio_analysis.h"

#include "audio_capture.h"
#include "mic_permission.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <numbers>
#include <thread>
#include <utility>

namespace rezonality
{
namespace
{

constexpr uint32_t kChannels = 2;
constexpr uint32_t kAnalysisFrames = 4096;
constexpr uint32_t kSpectrumBins = AudioTextureFrame::width;
constexpr float kDecibelRange = 110.0f;

using SampleWindow = std::array<float, kAnalysisFrames>;

void fft(std::vector<std::complex<float>>& values)
{
    const size_t count = values.size();
    for (size_t index = 1, reversed = 0; index < count; ++index)
    {
        size_t bit = count >> 1;
        for (; reversed & bit; bit >>= 1)
            reversed ^= bit;
        reversed ^= bit;
        if (index < reversed)
            std::swap(values[index], values[reversed]);
    }

    for (size_t length = 2; length <= count; length <<= 1)
    {
        const float angle = -2.0f * std::numbers::pi_v<float>
            / static_cast<float>(length);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (size_t first = 0; first < count; first += length)
        {
            std::complex<float> weight(1.0f, 0.0f);
            for (size_t offset = 0; offset < length / 2; ++offset)
            {
                const auto even = values[first + offset];
                const auto odd = values[first + offset + length / 2]
                    * weight;
                values[first + offset] = even + odd;
                values[first + offset + length / 2] = even - odd;
                weight *= step;
            }
        }
    }
}

std::array<float, kSpectrumBins> spectrum(const SampleWindow& samples)
{
    std::vector<std::complex<float>> transformed(kAnalysisFrames);
    float window_sum = 0.0f;
    for (uint32_t index = 0; index < kAnalysisFrames; ++index)
    {
        const float window = 0.54f - 0.46f * std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(index) / static_cast<float>(kAnalysisFrames - 1));
        transformed[index] = { samples[index] * window, 0.0f };
        window_sum += window;
    }
    fft(transformed);

    constexpr uint32_t available = kAnalysisFrames / 2 + 1;
    std::array<float, available> normalized{};
    for (uint32_t index = 0; index < available; ++index)
    {
        float power = std::norm(transformed[index])
            / std::max(window_sum * window_sum, 1e-12f);
        if (index != 0 && index != available - 1)
            power *= 2.0f;
        const float decibels = 10.0f * std::log10(std::max(power, 1e-20f));
        normalized[index] = std::clamp(
            1.0f + decibels / kDecibelRange, 0.0f, 1.0f);
    }

    std::array<float, kSpectrumBins> buckets{};
    for (uint32_t bucket = 0; bucket < kSpectrumBins; ++bucket)
    {
        const uint32_t first = 1 + (available - 1) * bucket / kSpectrumBins;
        const uint32_t last = std::max(first + 1,
            1 + (available - 1) * (bucket + 1) / kSpectrumBins);
        float sum = 0.0f;
        for (uint32_t index = first; index < last; ++index)
            sum += normalized[index];
        buckets[bucket] = sum / static_cast<float>(last - first);
    }

    const auto unsmoothed = buckets;
    for (uint32_t index = 2; index + 2 < kSpectrumBins; ++index)
        buckets[index] = (unsmoothed[index - 2]
                             + 2.0f * unsmoothed[index - 1]
                             + 3.0f * unsmoothed[index]
                             + 2.0f * unsmoothed[index + 1]
                             + unsmoothed[index + 2])
            / 9.0f;
    return buckets;
}

AudioTextureFrame make_texture(const SampleWindow& left,
    const SampleWindow& right, uint64_t generation, std::string status)
{
    const auto left_spectrum = spectrum(left);
    const auto right_spectrum = spectrum(right);
    AudioTextureFrame result;
    result.generation = generation;
    result.status = std::move(status);
    result.rgba.resize(AudioTextureFrame::width
            * AudioTextureFrame::height * 4,
        0.0f);
    const auto write_row = [&result](uint32_t row, const auto& values) {
        for (uint32_t column = 0; column < AudioTextureFrame::width;
             ++column)
        {
            const size_t pixel = (row * AudioTextureFrame::width + column)
                * 4;
            result.rgba[pixel] = values[column];
            result.rgba[pixel + 3] = 1.0f;
        }
    };
    write_row(0, left_spectrum);
    write_row(1, right_spectrum);

    std::array<float, AudioTextureFrame::width> left_wave{};
    std::array<float, AudioTextureFrame::width> right_wave{};
    for (uint32_t index = 0; index < AudioTextureFrame::width; ++index)
    {
        left_wave[index] = left[index];
        right_wave[index] = right[index];
    }
    write_row(2, left_wave);
    write_row(3, right_wave);
    return result;
}

AudioTextureFrame synthetic_frame()
{
    SampleWindow left{};
    SampleWindow right{};
    constexpr float sample_rate = 48000.0f;
    for (uint32_t index = 0; index < kAnalysisFrames; ++index)
    {
        const float time = static_cast<float>(index) / sample_rate;
        left[index] = 0.52f * std::sin(2.0f * std::numbers::pi_v<float> * 220.0f * time)
            + 0.18f * std::sin(2.0f * std::numbers::pi_v<float> * 880.0f * time);
        right[index] = 0.44f * std::sin(2.0f * std::numbers::pi_v<float> * 330.0f * time + 0.35f)
            + 0.22f * std::sin(2.0f * std::numbers::pi_v<float> * 1320.0f * time);
    }
    return make_texture(left, right, 1, "audio synthetic fixture");
}

} // namespace

namespace detail
{

struct AudioCaptureService::Impl
{
    Impl(std::string device_name,
        std::shared_ptr<const AudioCaptureOperations> operations)
        : device_name(std::move(device_name))
        , operations(std::move(operations))
        , frame(make_texture(left, right, 0, "audio opening input"))
        , opener([this](std::stop_token stop) { open(stop); })
    {
    }

    ~Impl()
    {
        opener.request_stop();
        if (opener.joinable())
            opener.join();

        AudioStreamHandle owned_stream = nullptr;
        {
            std::lock_guard lock(mutex);
            owned_stream = std::exchange(stream, nullptr);
        }
        if (owned_stream)
            operations->destroy(owned_stream);
    }

    static void append(SampleWindow& window,
        const std::vector<float>& interleaved, size_t frames,
        uint32_t channel)
    {
        const size_t used = std::min<size_t>(frames, kAnalysisFrames);
        if (used == 0)
            return;
        std::memmove(window.data(), window.data() + used,
            (kAnalysisFrames - used) * sizeof(float));
        const size_t source_first = frames - used;
        for (size_t index = 0; index < used; ++index)
            window[kAnalysisFrames - used + index]
                = interleaved[(source_first + index) * kChannels + channel];
    }

    std::string error() const
    {
        return operations->last_error ? operations->last_error()
                                      : "unknown device error";
    }

    void fail(std::string message)
    {
        std::lock_guard lock(mutex);
        status = "audio unavailable: " + std::move(message);
        frame.status = status;
    }

    void open(std::stop_token stop)
    {
        if (!operations->initialize())
        {
            fail("SDL audio init failed: " + error());
            return;
        }

        while (!stop.stop_requested())
        {
            const MicPermission current = operations->permission();
            if (current == MicPermission::Granted)
                break;
            if (current == MicPermission::Denied)
            {
                fail("microphone permission denied");
                return;
            }
            operations->wait(stop, std::chrono::milliseconds(100));
        }
        if (stop.stop_requested())
            return;

        AudioDeviceHandle device = operations->default_recording_device;
        std::string resolved_name = "default input";
        if (!device_name.empty())
        {
            device = 0;
            for (const AudioInputDevice& candidate
                : operations->recording_devices())
            {
                if (candidate.name == device_name)
                {
                    device = candidate.handle;
                    resolved_name = candidate.name;
                    break;
                }
            }
            if (device == 0)
            {
                fail("recording device '" + device_name + "' was not found");
                return;
            }
        }

        AudioStreamHandle opened = operations->open(device, stop);
        if (!opened)
        {
            if (!stop.stop_requested())
                fail("could not open " + resolved_name + ": " + error());
            return;
        }

        std::lock_guard lock(mutex);
        if (stop.stop_requested())
        {
            operations->destroy(opened);
            return;
        }
        stream = opened;
        status = "audio live: " + resolved_name + " (shared capture)";
        frame.status = status;
        if (visible_clients > 0 && !operations->resume(stream))
        {
            status = "audio unavailable: resume failed: " + error();
            frame.status = status;
            operations->destroy(stream);
            stream = nullptr;
        }
    }

    std::string device_name;
    std::shared_ptr<const AudioCaptureOperations> operations;
    std::mutex mutex;
    AudioStreamHandle stream = nullptr;
    size_t visible_clients = 0;
    SampleWindow left{};
    SampleWindow right{};
    std::vector<float> samples;
    uint64_t generation = 0;
    std::string status = "audio opening input";
    AudioTextureFrame frame;
    std::jthread opener;
};

AudioCaptureService::AudioCaptureService(std::string device_name,
    std::shared_ptr<const AudioCaptureOperations> operations)
    : impl_(std::make_unique<Impl>(
          std::move(device_name), std::move(operations)))
{
}

AudioCaptureService::~AudioCaptureService() = default;

void AudioCaptureService::add_visible()
{
    std::lock_guard lock(impl_->mutex);
    ++impl_->visible_clients;
    if (impl_->stream && impl_->visible_clients == 1
        && !impl_->operations->resume(impl_->stream))
    {
        impl_->status = "audio unavailable: resume failed: "
            + impl_->error();
        impl_->frame.status = impl_->status;
        impl_->operations->destroy(impl_->stream);
        impl_->stream = nullptr;
    }
}

void AudioCaptureService::remove_visible()
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->visible_clients == 0)
        return;
    --impl_->visible_clients;
    if (impl_->stream && impl_->visible_clients == 0)
    {
        impl_->operations->pause(impl_->stream);
        impl_->operations->clear(impl_->stream);
    }
}

AudioTextureFrame AudioCaptureService::frame()
{
    std::lock_guard lock(impl_->mutex);
    if (!impl_->stream || impl_->visible_clients == 0)
        return impl_->frame;

    const int available = impl_->operations->available(impl_->stream);
    if (available <= 0)
        return impl_->frame;
    if (available > max_buffered_audio_bytes)
    {
        impl_->operations->clear(impl_->stream);
        return impl_->frame;
    }

    impl_->samples.resize(static_cast<size_t>(available) / sizeof(float));
    const int bytes = impl_->operations->read(impl_->stream,
        impl_->samples.data(),
        static_cast<int>(impl_->samples.size() * sizeof(float)));
    if (bytes <= 0)
        return impl_->frame;
    const size_t frames = static_cast<size_t>(bytes)
        / (sizeof(float) * kChannels);
    Impl::append(impl_->left, impl_->samples, frames, 0);
    Impl::append(impl_->right, impl_->samples, frames, 1);
    impl_->frame = make_texture(
        impl_->left, impl_->right, ++impl_->generation, impl_->status);
    return impl_->frame;
}

struct AudioCaptureRegistry::Impl
{
    explicit Impl(AudioCaptureOperations configured)
        : operations(std::make_shared<AudioCaptureOperations>(
              std::move(configured)))
    {
    }

    std::shared_ptr<const AudioCaptureOperations> operations;
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<AudioCaptureService>> services;
};

AudioCaptureRegistry::AudioCaptureRegistry(AudioCaptureOperations operations)
    : impl_(std::make_unique<Impl>(std::move(operations)))
{
}

AudioCaptureRegistry::~AudioCaptureRegistry() = default;

std::shared_ptr<AudioCaptureService> AudioCaptureRegistry::capture(
    const std::string& device_name)
{
    std::lock_guard lock(impl_->mutex);
    auto& slot = impl_->services[device_name];
    auto service = slot.lock();
    if (!service)
    {
        service = std::shared_ptr<AudioCaptureService>(
            new AudioCaptureService(device_name, impl_->operations));
        slot = service;
    }
    return service;
}

} // namespace detail

namespace
{

detail::AudioCaptureOperations production_capture_operations()
{
    detail::AudioCaptureOperations operations;
    operations.default_recording_device
        = static_cast<detail::AudioDeviceHandle>(
            SDL_AUDIO_DEVICE_DEFAULT_RECORDING);
    operations.initialize = [] {
        return SDL_WasInit(SDL_INIT_AUDIO)
            || SDL_InitSubSystem(SDL_INIT_AUDIO);
    };
    operations.permission = [] { return query_mic_permission(); };
    operations.wait = [](std::stop_token stop,
                          std::chrono::milliseconds duration) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        wake.wait_for(lock, stop, duration, [] { return false; });
    };
    operations.recording_devices = [] {
        std::vector<detail::AudioInputDevice> result;
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
        for (int index = 0; devices && index < count; ++index)
        {
            const char* name = SDL_GetAudioDeviceName(devices[index]);
            if (name)
            {
                result.push_back({
                    static_cast<detail::AudioDeviceHandle>(devices[index]),
                    name,
                });
            }
        }
        SDL_free(devices);
        return result;
    };
    operations.open = [](detail::AudioDeviceHandle device,
                          std::stop_token) -> detail::AudioStreamHandle {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = kChannels;
        spec.freq = 48000;
        return SDL_OpenAudioDeviceStream(
            static_cast<SDL_AudioDeviceID>(device), &spec, nullptr, nullptr);
    };
    operations.resume = [](detail::AudioStreamHandle stream) {
        return SDL_ResumeAudioStreamDevice(
            static_cast<SDL_AudioStream*>(stream));
    };
    operations.pause = [](detail::AudioStreamHandle stream) {
        SDL_PauseAudioStreamDevice(static_cast<SDL_AudioStream*>(stream));
    };
    operations.clear = [](detail::AudioStreamHandle stream) {
        SDL_ClearAudioStream(static_cast<SDL_AudioStream*>(stream));
    };
    operations.available = [](detail::AudioStreamHandle stream) {
        return SDL_GetAudioStreamAvailable(
            static_cast<SDL_AudioStream*>(stream));
    };
    operations.read = [](detail::AudioStreamHandle stream, float* samples,
                          int bytes) {
        return SDL_GetAudioStreamData(
            static_cast<SDL_AudioStream*>(stream), samples, bytes);
    };
    operations.destroy = [](detail::AudioStreamHandle stream) {
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(stream));
    };
    operations.last_error = [] { return std::string(SDL_GetError()); };
    return operations;
}

detail::AudioCaptureRegistry& production_capture_registry()
{
    static detail::AudioCaptureRegistry registry(
        production_capture_operations());
    return registry;
}

} // namespace

struct AudioAnalyzer::Impl
{
    explicit Impl(AudioOptions configured)
        : options(std::move(configured))
    {
        if (options.source == AudioOptions::Source::Input)
        {
            capture = production_capture_registry().capture(
                options.device_name);
            capture->add_visible();
        }
        else if (options.source == AudioOptions::Source::Synthetic)
            fixed = synthetic_frame();
        else
        {
            SampleWindow silence{};
            fixed = make_texture(silence, silence, 1,
                "audio unavailable: silent fallback requested");
        }
    }

    ~Impl()
    {
        if (capture && visible)
            capture->remove_visible();
    }

    AudioOptions options;
    std::shared_ptr<detail::AudioCaptureService> capture;
    AudioTextureFrame fixed;
    bool visible = true;
};

AudioAnalyzer::AudioAnalyzer(AudioOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

AudioAnalyzer::~AudioAnalyzer() = default;

void AudioAnalyzer::set_visible(bool visible)
{
    if (impl_->visible == visible)
        return;
    impl_->visible = visible;
    if (!impl_->capture)
        return;
    if (visible)
        impl_->capture->add_visible();
    else
        impl_->capture->remove_visible();
}

AudioTextureFrame AudioAnalyzer::frame()
{
    return impl_->capture ? impl_->capture->frame() : impl_->fixed;
}

} // namespace rezonality
