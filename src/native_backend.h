#pragma once

#include "audio_types.h"
#include "camera.h"
#include "runtime_controller.h"

#include <draxul/plugin_api.h>

#include <memory>

namespace rezonality
{

// Platform-selected native implementation. The controller owns candidate and
// activation policy; this class owns preparation, recording, and native resource
// retirement for the selected backend.
class NativeBackend
{
public:
    NativeBackend();
    ~NativeBackend();

    NativeBackend(const NativeBackend&) = delete;
    NativeBackend& operator=(const NativeBackend&) = delete;

#if defined(__APPLE__)
    void bind_frame(const DraxulPluginMetalFrameV2& frame,
        double animation_seconds, const Camera& camera);
    DraxulPluginRenderResultV2 record(
        const DraxulPluginMetalFrameV2& frame,
        const AudioTextureFrame* audio, bool paused);
#else
    void bind_frame(const DraxulPluginVulkanFrameV2& frame,
        double animation_seconds, const Camera& camera);
    DraxulPluginRenderResultV2 record(
        const DraxulPluginVulkanFrameV2& frame,
        const AudioTextureFrame* audio, bool paused);
#endif

    [[nodiscard]] bool active_compatible() const;
    BackendPreparation prepare(const ShaderBuild& build);
    void activate_prepared();
    void retire_completed_slot(uint32_t frame_index);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rezonality
