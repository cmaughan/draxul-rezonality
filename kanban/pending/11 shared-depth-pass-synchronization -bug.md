# Synchronize reused depth images

**Summary:** Synchronize shared depth images so successive drawing passes preserve correct visibility.

**Priority:** P1  
**Source:** `plugins/rezonality/src/native_backend_vulkan.cpp`

**Evidence and trigger:** B25; the shipped robot scene reuses depth across clear/load/test/write passes, but dependencies cover only color and sampling.

- [x] **Investigate:** Trace depth initialization, pass dependencies, loads/stores, and reuse across frames.
  - `examples/pbr_robot` and `examples/robot2`: `Sky` clears `Color`+`Depth`, then `Robot` loads `Depth` and depth-tests/writes it; the next frame's `Sky` clears it again. Depth surfaces stay in `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` (no layout transition), so ordering relies entirely on the offscreen render pass's external subpass dependencies.
  - Those dependencies covered only `FRAGMENT_SHADER`/`SHADER_READ` -> `COLOR_ATTACHMENT_OUTPUT`/`COLOR_ATTACHMENT_WRITE` and the reverse. Depth load/clear (early fragment tests) and depth store (late fragment tests) were outside every scope, so `Robot`'s depth load/test raced `Sky`'s clear (and the next frame's clear raced `Robot`'s depth writes). Color `LOAD_OP_LOAD` reads (`COLOR_ATTACHMENT_READ`) and color write-after-write between passes sharing a target were also outside the access scopes.
  - The one-time depth initialization barrier only targeted `EARLY_FRAGMENT_TESTS`/`DEPTH_STENCIL_ATTACHMENT_WRITE`, not a first-pass depth load or late-test write.
  - Metal: render targets are hazard-tracked `MTLTexture`s, so successive render encoders in the borrowed command buffer are ordered by the driver; no equivalent defect and no Metal change.
- [x] **Fix:** Add the necessary depth stages and read/write dependencies without weakening color synchronization.
  - Both external dependencies of every offscreen render pass now include color-attachment output and early/late fragment-test stages, with color and depth attachment read/write access, in addition to the existing fragment-shader sampling scope. The depth initialization barrier now covers both depth-test stages with read and write access.
- [ ] **Acceptance:** The robot scene and another shared-depth case pass synchronization validation and preserve expected occlusion.
  - Windows/Vulkan (not runnable on macOS): run `rezonality-pbr-robot` and `rezonality-deferred-shading` (and `examples/robot2`) with `VK_LAYER_KHRONOS_validation` and synchronization validation enabled (`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`, or `khronos_validation.validate_sync = true`); expect no `SYNC-HAZARD-*` reports for Rezonality depth/color attachments across several frames and a pane resize, and the robot correctly occluding itself against the sky.
  - macOS/Metal: the `rezonality-pbr-robot` and `rezonality-deferred-shading` render goldens pass unchanged in the Rezonality aggregate (Metal code is unchanged).
- [ ] **Validation:** Preserve Metal behavior; run the Rezonality-scoped aggregate, Vulkan rendering checks, and same-cache smoke.
  - macOS (2026-10-08, with cards 04/06 on top): `python3 do.py test debug --rezonality` — all seven Rezonality render goldens (including `rezonality-pbr-robot` and `rezonality-deferred-shading`) and the Rezonality runtime/audio/layout/Neovim suites passed; the Rezonality contract and project suites failed only under concurrent multi-agent load (shared `/tmp` fixture paths, watcher timing) and passed when rerun in isolation. `python3 do.py smoke debug --skip-build` passed.
  - The Vulkan file was type-checked on macOS with `clang++ -fsyntax-only` against Vulkan 1.4 headers and VMA 3.1.0, but it is not compiled or run by the macOS build. Windows: build, run the Rezonality render goldens (`py do.py test debug --rezonality`) and the validation-layer check above.
