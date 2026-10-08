# Retire one-time Rezonality Vulkan texture staging

**Summary:** Release temporary texture-upload memory after the graphics card has finished using it so Rezonality on Windows does not retain unnecessary copies of static images.

**Source:** `plugins/rezonality/src/native_backend_vulkan.cpp`  
**Priority:** P2; static, high confidence. **Reported by:** Claude, Codex. Lines 556–559 and 634–638 retain mapped staging buffers after one-time texture upload at 1875–1881 and 1934–1973; lines 2206–2223 retire whole generations only. A 4096² RGBA buffer represents 64 MiB of staging by arithmetic, not an observed allocation.

- [ ] **Baseline:** Measure static staging bytes after initial frames, after reload, and at generation retirement.
  - Confirmed in source: `create_surface()` and `create_model_texture()` keep a persistently mapped `HostSequentialWrite` staging buffer per static image and per model material texture, and `initialize_generation_images()` copied from it once but left it owned by the resource until `destroy_generation()`. For `examples/pbr_robot` that is the full image/texture pixel payload of every live generation (the Metal measurement of the same scene's asset bytes is ~817 MB including vertices/indices; the staging share is its texture portion).
  - `NativeBackend::resource_stats().retained_upload_staging_bytes` now reports the live one-time staging bytes. Measuring it on Vulkan (after first frames, after reload, after retirement) needs Windows and is still open.
- [x] **Implement:** Retire completed one-time upload buffers with their submitting frame slot; retain streaming audio buffers.
  - After recording each static surface or model-texture copy, `initialize_generation_images()` moves the staging buffer into `BackendState::retired_uploads` tagged with the recording frame slot. `retire_completed_slot()` (called when that slot's previous submission has completed, the same contract as generation retirement) destroys it. Audio surfaces' per-slot `audio_upload_buffers` are unchanged. Backend teardown destroys any pending uploads before the allocator.
  - Slots outside the 64-bit retirement mask keep the buffer on its resource, which releases it at generation retirement as before.
  - With card `04 rezonality-resize-generation-reuse -perf.md`, static images and model textures are shared across size-only generations, so a resize neither re-creates nor re-retains their staging.
- [ ] **Functional safety:** Cover aborted recording, failed submit, in-flight completion, and visible texture integrity.
  - In-flight completion: the buffer is destroyed only by `retire_completed_slot()` for the slot whose command buffer recorded the copy, so the copy has executed first.
  - Aborted recording / failed submit: the plugin ABI reports only slot completion, so a frame Draxul discards after `record()` is indistinguishable from a completed one. The image's `initialized` flag was already set at record time before this change, so such a frame never re-uploaded; releasing the staging changes nothing there. Verifying Draxul's behavior for a discarded frame on Windows (and visible texture integrity after the first frames) remains open.
- [ ] **Compare:** Require static staging to return to steady baseline after upload completion.
  - Windows: `retained_upload_staging_bytes` should fall to 0 after `buffered_frame_count` frames following each load or reload (audio upload buffers are excluded by design).
- [ ] **Platforms:** Run Windows/Vulkan validation and Rezonality renders; inspect Metal’s separate upload policy.
  - Metal inspected: static images and model textures are created `MTLStorageModeManaged` and filled with `replaceRegion`, so there is no plugin-owned staging buffer to retire (the Metal resize test asserts `retained_upload_staging_bytes == 0`). On discrete-GPU Macs the driver keeps a managed CPU mirror; moving to private textures with a blit upload would be a separate change.
  - Windows/Vulkan (not compiled or run on macOS; type-checked with `clang++ -fsyntax-only` against Vulkan 1.4 headers and VMA 3.1.0): run `py do.py test debug --rezonality` (render goldens `rezonality-pbr-robot`, `rezonality-deferred-shading`, `rezonality-protoplanetary-disc`, `rezonality-ray-tracer`) with `VK_LAYER_KHRONOS_validation` enabled and check for no use-after-free/`vkDestroyBuffer`-in-use errors on `rezonality-texture-upload` or `rezonality-model-texture-upload` buffers, and correct textures after the first frames and after a live reload.
- [ ] **Acceptance:** Uploaded static textures do not retain their one-time mapped staging.
  - Implemented; pending the Windows measurement and validation above.
