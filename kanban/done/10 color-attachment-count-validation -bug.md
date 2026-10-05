# Reject unsupported color-output counts

**Summary:** Reject excessive color outputs so an invalid scene cannot crash the graphics backend.

**Priority:** 10  
**Severity:** CRITICAL  
**Source:** `plugins/rezonality/src/native_backend_metal.mm`

**Evidence and trigger:** B08; unchecked scene targets exceed Metal’s fixed slots, and Vulkan also lacks a device-limit check.

- [x] **Investigate:** Trace target counting and validation before pipeline preparation and recording.
  Confirmed: Metal indexed `colorAttachments[color_index++]` for every non-depth target and aborted with `attachmentIndex(8) must be < 8` for a nine-target pass (reproduced by the new test before the fix). Vulkan passed any count to `vkCreateRenderPass` without consulting `maxColorAttachments`.
- [x] **Fix:** Reject unsupported counts before native indexing, using each backend’s actual limit.
  Shared `validate_color_target_count` in `src/native_backend_helpers.h`; Metal checks against the fixed eight-slot limit before pipeline preparation, and Vulkan stores `min(maxColorAttachments, maxFragmentOutputAttachments)` per generation and checks before creating the render pass.
- [x] **Acceptance:** A nine-color Metal candidate and Vulkan candidates exceeding the device limit fail with diagnostics while retaining the active scene.
  Metal is covered by `Rezonality Metal accepts the color-target limit and rejects more` (`tests/rezonality_metal_backend_tests.mm`) against a real system device. The Vulkan path was reviewed but not compiled locally; Windows CI covers the build. A rejected preparation flows through the existing failed-preparation path that keeps the active generation.
- [x] **Validation:** Check accepted boundary counts; run the Rezonality-scoped aggregate, both backend checks, and same-cache smoke.
  Eight Metal color targets prepare successfully; nine are rejected. Vulkan has no local device test on macOS.
