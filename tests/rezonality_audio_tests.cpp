#include <catch2/catch_test_macros.hpp>

#include "audio_analysis.h"
#include "audio_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;

struct FakeCaptureDevice
{
    bool initialize_result = true;
    bool open_result = true;
    bool resume_result = true;
    bool block_open = false;
    std::string error = "injected failure";
    std::vector<rezonality::MicPermission> permissions{
        rezonality::MicPermission::Granted,
    };
    std::vector<rezonality::detail::AudioInputDevice> devices;
    std::atomic<size_t> permission_index{ 0 };
    std::atomic<int> initialize_calls{ 0 };
    std::atomic<int> permission_calls{ 0 };
    std::atomic<int> wait_calls{ 0 };
    std::atomic<int> device_calls{ 0 };
    std::atomic<int> open_calls{ 0 };
    std::atomic<int> resume_calls{ 0 };
    std::atomic<int> pause_calls{ 0 };
    std::atomic<int> clear_calls{ 0 };
    std::atomic<int> available_calls{ 0 };
    std::atomic<int> read_calls{ 0 };
    std::atomic<int> destroy_calls{ 0 };
    std::atomic<int> available_bytes{ 0 };
    std::mutex open_mutex;
    std::condition_variable_any open_wake;
    bool open_entered = false;

    rezonality::detail::AudioCaptureOperations operations()
    {
        rezonality::detail::AudioCaptureOperations result;
        result.default_recording_device = 1;
        result.initialize = [this] {
            ++initialize_calls;
            return initialize_result;
        };
        result.permission = [this] {
            ++permission_calls;
            const size_t index = permission_index.fetch_add(1);
            return permissions[std::min(index, permissions.size() - 1)];
        };
        result.wait = [this](std::stop_token, std::chrono::milliseconds) {
            ++wait_calls;
        };
        result.recording_devices = [this] {
            ++device_calls;
            return devices;
        };
        result.open = [this](rezonality::detail::AudioDeviceHandle,
                              std::stop_token stop) {
            const int call = ++open_calls;
            if (block_open)
            {
                std::unique_lock lock(open_mutex);
                open_entered = true;
                open_wake.notify_all();
                open_wake.wait(lock, stop, [] { return false; });
            }
            if (!open_result)
                return static_cast<rezonality::detail::AudioStreamHandle>(
                    nullptr);
            return reinterpret_cast<rezonality::detail::AudioStreamHandle>(
                static_cast<uintptr_t>(call + 16));
        };
        result.resume = [this](rezonality::detail::AudioStreamHandle) {
            ++resume_calls;
            return resume_result;
        };
        result.pause = [this](rezonality::detail::AudioStreamHandle) {
            ++pause_calls;
        };
        result.clear = [this](rezonality::detail::AudioStreamHandle) {
            ++clear_calls;
        };
        result.available = [this](rezonality::detail::AudioStreamHandle) {
            ++available_calls;
            return available_bytes.load();
        };
        result.read = [this](rezonality::detail::AudioStreamHandle,
                              float*, int bytes) {
            ++read_calls;
            return bytes;
        };
        result.destroy = [this](rezonality::detail::AudioStreamHandle) {
            ++destroy_calls;
        };
        result.last_error = [this] { return error; };
        return result;
    }

    bool wait_for_open()
    {
        std::unique_lock lock(open_mutex);
        return open_wake.wait_for(lock, 1s, [this] { return open_entered; });
    }
};

rezonality::AudioTextureFrame wait_for_status(
    const std::shared_ptr<rezonality::detail::AudioCaptureService>& capture,
    const std::string& expected)
{
    rezonality::AudioTextureFrame result;
    for (int attempt = 0; attempt < 1000; ++attempt)
    {
        result = capture->frame();
        if (result.status.find(expected) != std::string::npos)
            return result;
        std::this_thread::sleep_for(1ms);
    }
    return result;
}

} // namespace

TEST_CASE("Rezonality capture handles permission and initialization outcomes",
    "[rezonality][audio]")
{
    SECTION("initialization failure")
    {
        FakeCaptureDevice device;
        device.initialize_result = false;
        rezonality::detail::AudioCaptureRegistry registry(
            device.operations());
        auto capture = registry.capture("");
        capture->add_visible();
        CHECK(wait_for_status(capture, "SDL audio init failed").status
            == "audio unavailable: SDL audio init failed: injected failure");
        CHECK(device.permission_calls == 0);
        CHECK(device.open_calls == 0);
    }

    SECTION("pending then granted")
    {
        FakeCaptureDevice device;
        device.permissions = {
            rezonality::MicPermission::Pending,
            rezonality::MicPermission::Pending,
            rezonality::MicPermission::Granted,
        };
        rezonality::detail::AudioCaptureRegistry registry(
            device.operations());
        auto capture = registry.capture("");
        capture->add_visible();
        CHECK(wait_for_status(capture, "audio live").status
            == "audio live: default input (shared capture)");
        CHECK(device.permission_calls == 3);
        CHECK(device.wait_calls == 2);
        CHECK(device.open_calls == 1);
        CHECK(device.resume_calls == 1);
    }

    SECTION("denied")
    {
        FakeCaptureDevice device;
        device.permissions = { rezonality::MicPermission::Denied };
        rezonality::detail::AudioCaptureRegistry registry(
            device.operations());
        auto capture = registry.capture("");
        capture->add_visible();
        CHECK(wait_for_status(capture, "permission denied").status
            == "audio unavailable: microphone permission denied");
        CHECK(device.open_calls == 0);
    }
}

TEST_CASE("Rezonality capture reports lookup open and resume failures",
    "[rezonality][audio]")
{
    SECTION("device lookup")
    {
        FakeCaptureDevice device;
        device.devices = { { 7, "another input" } };
        rezonality::detail::AudioCaptureRegistry registry(
            device.operations());
        auto capture = registry.capture("requested input");
        capture->add_visible();
        CHECK(wait_for_status(capture, "was not found").status
            == "audio unavailable: recording device 'requested input' was not found");
        CHECK(device.device_calls == 1);
        CHECK(device.open_calls == 0);
    }

    SECTION("open")
    {
        FakeCaptureDevice device;
        device.open_result = false;
        rezonality::detail::AudioCaptureRegistry registry(
            device.operations());
        auto capture = registry.capture("");
        capture->add_visible();
        CHECK(wait_for_status(capture, "could not open").status
            == "audio unavailable: could not open default input: injected failure");
        CHECK(device.open_calls == 1);
        CHECK(device.destroy_calls == 0);
    }

    SECTION("resume cleans up exactly once")
    {
        FakeCaptureDevice device;
        device.resume_result = false;
        {
            rezonality::detail::AudioCaptureRegistry registry(
                device.operations());
            auto capture = registry.capture("");
            capture->add_visible();
            CHECK(wait_for_status(capture, "resume failed").status
                == "audio unavailable: resume failed: injected failure");
            CHECK(device.resume_calls == 1);
            CHECK(device.destroy_calls == 1);
        }
        CHECK(device.destroy_calls == 1);
    }

    SECTION("resume after an initially hidden open")
    {
        FakeCaptureDevice device;
        device.resume_result = false;
        {
            rezonality::detail::AudioCaptureRegistry registry(
                device.operations());
            auto capture = registry.capture("");
            REQUIRE(wait_for_status(capture, "audio live").status
                == "audio live: default input (shared capture)");
            capture->add_visible();
            CHECK(capture->frame().status
                == "audio unavailable: resume failed: injected failure");
            CHECK(device.resume_calls == 1);
            CHECK(device.destroy_calls == 1);
        }
        CHECK(device.destroy_calls == 1);
    }
}

TEST_CASE("Rezonality capture cancellation destroys a late-opened stream once",
    "[rezonality][audio]")
{
    FakeCaptureDevice device;
    device.block_open = true;
    {
        rezonality::detail::AudioCaptureRegistry registry(device.operations());
        auto capture = registry.capture("");
        capture->add_visible();
        REQUIRE(device.wait_for_open());
        capture.reset();
    }
    CHECK(device.open_calls == 1);
    CHECK(device.resume_calls == 0);
    CHECK(device.destroy_calls == 1);
}

TEST_CASE("Rezonality capture registry shares exact devices and visibility",
    "[rezonality][audio]")
{
    FakeCaptureDevice device;
    device.devices = { { 7, "shared" }, { 9, "distinct" } };
    rezonality::detail::AudioCaptureRegistry registry(device.operations());
    auto first = registry.capture("shared");
    auto second = registry.capture("shared");
    auto distinct = registry.capture("distinct");
    REQUIRE(first == second);
    CHECK(first != distinct);

    first->add_visible();
    second->add_visible();
    CHECK(wait_for_status(first, "audio live").status
        == "audio live: shared (shared capture)");
    CHECK(wait_for_status(distinct, "audio live").status
        == "audio live: distinct (shared capture)");
    const int resumes_after_open = device.resume_calls.load();

    first->remove_visible();
    CHECK(device.pause_calls == 0);
    CHECK(device.clear_calls == 0);
    second->remove_visible();
    CHECK(device.pause_calls == 1);
    CHECK(device.clear_calls == 1);

    first->add_visible();
    CHECK(device.resume_calls == resumes_after_open + 1);
    first->remove_visible();
    CHECK(device.pause_calls == 2);
    CHECK(device.clear_calls == 2);
}

TEST_CASE("Rezonality capture clears bounded backlog before reading",
    "[rezonality][audio]")
{
    FakeCaptureDevice device;
    device.available_bytes
        = rezonality::detail::max_buffered_audio_bytes + 4;
    rezonality::detail::AudioCaptureRegistry registry(device.operations());
    auto capture = registry.capture("");
    capture->add_visible();
    REQUIRE(wait_for_status(capture, "audio live").status
        == "audio live: default input (shared capture)");

    const int clears_before_frame = device.clear_calls.load();
    const auto frame = capture->frame();
    CHECK(frame.generation == 0);
    CHECK(device.clear_calls == clears_before_frame + 1);
    CHECK(device.read_calls == 0);
}

TEST_CASE("Rezonality synthetic and silent audio are deterministic",
    "[rezonality][audio]")
{
    rezonality::AudioOptions options;
    options.source = rezonality::AudioOptions::Source::Synthetic;
    rezonality::AudioAnalyzer analyzer(options);

    const auto first = analyzer.frame();
    const auto second = analyzer.frame();
    REQUIRE(first.rgba.size()
        == rezonality::AudioTextureFrame::width
            * rezonality::AudioTextureFrame::height * 4);
    CHECK(first.generation == 1);
    CHECK(first.status == "audio synthetic fixture");
    CHECK(first.rgba == second.rgba);
    CHECK(std::any_of(first.rgba.begin(), first.rgba.end(),
        [](float value) { return value > 0.1f && value < 0.99f; }));

    analyzer.set_visible(false);
    CHECK(analyzer.frame().rgba == first.rgba);
    analyzer.set_visible(true);
    CHECK(analyzer.frame().rgba == first.rgba);

    options.source = rezonality::AudioOptions::Source::Silent;
    rezonality::AudioAnalyzer silent(options);
    const auto fallback = silent.frame();
    CHECK(fallback.generation == 1);
    CHECK(fallback.status.find("audio unavailable") != std::string::npos);
    CHECK(fallback.rgba.size() == first.rgba.size());
    CHECK(std::all_of(fallback.rgba.begin(), fallback.rgba.end(),
        [](float value) { return value == 0.0f || value == 1.0f; }));
}
