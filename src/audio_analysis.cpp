#include "audio_analysis.h"

#include "mic_permission.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <mutex>
#include <numbers>
#include <thread>

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

class CaptureService final
{
public:
    explicit CaptureService(std::string device_name)
        : device_name_(std::move(device_name))
        , frame_(make_texture(left_, right_, 0, "audio opening input"))
        , opener_([this](std::stop_token stop) { open(stop); })
    {
    }

    ~CaptureService()
    {
        opener_.request_stop();
        if (opener_.joinable())
            opener_.join();
        std::lock_guard lock(mutex_);
        if (stream_)
            SDL_DestroyAudioStream(stream_);
    }

    void add_visible()
    {
        std::lock_guard lock(mutex_);
        ++visible_clients_;
        if (stream_ && visible_clients_ == 1)
            SDL_ResumeAudioStreamDevice(stream_);
    }

    void remove_visible()
    {
        std::lock_guard lock(mutex_);
        if (visible_clients_ == 0)
            return;
        --visible_clients_;
        if (stream_ && visible_clients_ == 0)
        {
            SDL_PauseAudioStreamDevice(stream_);
            SDL_ClearAudioStream(stream_);
        }
    }

    AudioTextureFrame frame()
    {
        std::lock_guard lock(mutex_);
        if (!stream_ || visible_clients_ == 0)
            return frame_;

        const int available = SDL_GetAudioStreamAvailable(stream_);
        if (available <= 0)
            return frame_;
        if (available > 48000 * static_cast<int>(kChannels)
                * static_cast<int>(sizeof(float)) * 3)
        {
            SDL_ClearAudioStream(stream_);
            return frame_;
        }

        samples_.resize(static_cast<size_t>(available) / sizeof(float));
        const int bytes = SDL_GetAudioStreamData(stream_, samples_.data(),
            static_cast<int>(samples_.size() * sizeof(float)));
        if (bytes <= 0)
            return frame_;
        const size_t frames = static_cast<size_t>(bytes)
            / (sizeof(float) * kChannels);
        append(left_, samples_, frames, 0);
        append(right_, samples_, frames, 1);
        frame_ = make_texture(left_, right_, ++generation_, status_);
        return frame_;
    }

private:
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

    void fail(std::string error)
    {
        std::lock_guard lock(mutex_);
        status_ = "audio unavailable: " + std::move(error);
        frame_.status = status_;
    }

    void open(std::stop_token stop)
    {
        if (!SDL_WasInit(SDL_INIT_AUDIO)
            && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            fail(std::string("SDL audio init failed: ") + SDL_GetError());
            return;
        }

        while (!stop.stop_requested())
        {
            const MicPermission permission = query_mic_permission();
            if (permission == MicPermission::Granted)
                break;
            if (permission == MicPermission::Denied)
            {
                fail("microphone permission denied");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stop.stop_requested())
            return;

        SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
        std::string resolved_name = "default input";
        if (!device_name_.empty())
        {
            int count = 0;
            SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
            device = 0;
            for (int index = 0; devices && index < count; ++index)
            {
                const char* name = SDL_GetAudioDeviceName(devices[index]);
                if (name && device_name_ == name)
                {
                    device = devices[index];
                    resolved_name = name;
                    break;
                }
            }
            SDL_free(devices);
            if (device == 0)
            {
                fail("recording device '" + device_name_ + "' was not found");
                return;
            }
        }

        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = kChannels;
        spec.freq = 48000;
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
            device, &spec, nullptr, nullptr);
        if (!stream)
        {
            fail(std::string("could not open ") + resolved_name + ": "
                + SDL_GetError());
            return;
        }

        std::lock_guard lock(mutex_);
        if (stop.stop_requested())
        {
            SDL_DestroyAudioStream(stream);
            return;
        }
        stream_ = stream;
        status_ = "audio live: " + resolved_name + " (shared capture)";
        frame_.status = status_;
        if (visible_clients_ > 0
            && !SDL_ResumeAudioStreamDevice(stream_))
        {
            status_ = std::string("audio unavailable: resume failed: ")
                + SDL_GetError();
            frame_.status = status_;
            SDL_DestroyAudioStream(stream_);
            stream_ = nullptr;
        }
    }

    std::string device_name_;
    std::mutex mutex_;
    SDL_AudioStream* stream_ = nullptr;
    size_t visible_clients_ = 0;
    SampleWindow left_{};
    SampleWindow right_{};
    std::vector<float> samples_;
    uint64_t generation_ = 0;
    std::string status_ = "audio opening input";
    AudioTextureFrame frame_;
    std::jthread opener_;
};

std::shared_ptr<CaptureService> shared_capture(const std::string& device)
{
    static std::mutex mutex;
    static std::map<std::string, std::weak_ptr<CaptureService>> services;
    std::lock_guard lock(mutex);
    auto& slot = services[device];
    auto service = slot.lock();
    if (!service)
    {
        service = std::make_shared<CaptureService>(device);
        slot = service;
    }
    return service;
}

} // namespace

struct AudioAnalyzer::Impl
{
    explicit Impl(AudioOptions configured)
        : options(std::move(configured))
    {
        if (options.source == AudioOptions::Source::Input)
        {
            capture = shared_capture(options.device_name);
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
    std::shared_ptr<CaptureService> capture;
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
