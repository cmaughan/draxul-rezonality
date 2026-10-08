# Synchronize reused depth images

**Summary:** Synchronize shared depth images so successive drawing passes preserve correct visibility.

**Priority:** P1 — shared attachment hazards can corrupt live scene visibility.
**Source:** `plugins/rezonality/src/native_backend_vulkan.cpp`

**Evidence and trigger:** B25; the shipped robot scene reuses depth across clear/load/test/write passes, but dependencies cover only color and sampling.

- [x] **Investigate:** Trace depth initialization, pass dependencies, loads/stores, and reuse across frames.
  - `examples/pbr_robot` and `examples/robot2`: `Sky` clears `Color`+`Depth`, then `Robot` loads `Depth` and depth-tests/writes it; the next frame's `Sky` clears it again. Depth surfaces stay in `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` (no layout transition), so ordering relies entirely on the offscreen render pass's external subpass dependencies.
  - Those dependencies covered only `FRAGMENT_SHADER`/`SHADER_READ` -> `COLOR_ATTACHMENT_OUTPUT`/`COLOR_ATTACHMENT_WRITE` and the reverse. Depth load/clear (early fragment tests) and depth store (late fragment tests) were outside every scope, so `Robot`'s depth load/test raced `Sky`'s clear (and the next frame's clear raced `Robot`'s depth writes). Color `LOAD_OP_LOAD` reads (`COLOR_ATTACHMENT_READ`) and color write-after-write between passes sharing a target were also outside the access scopes.
  - The one-time depth initialization barrier only targeted `EARLY_FRAGMENT_TESTS`/`DEPTH_STENCIL_ATTACHMENT_WRITE`, not a first-pass depth load or late-test write.
  - Metal: render targets are hazard-tracked `MTLTexture`s, so successive render encoders in the borrowed command buffer are ordered by the driver; no equivalent defect and no Metal change.
- [x] **Fix:** Add the necessary depth stages and read/write dependencies without weakening color synchronization.
  - Both external dependencies of every offscreen render pass now include color-attachment output and early/late fragment-test stages, with color and depth attachment read/write access, in addition to the existing fragment-shader sampling scope. The depth initialization barrier now covers both depth-test stages with read and write access.
- [x] **Acceptance:** The robot scene and another shared-depth case pass synchronization validation and preserve expected occlusion.
  - Windows/Vulkan (not runnable on macOS): run `rezonality-pbr-robot` and `rezonality-deferred-shading` (and `examples/robot2`) with `VK_LAYER_KHRONOS_validation` and synchronization validation enabled (`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`, or `khronos_validation.validate_sync = true`); expect no `SYNC-HAZARD-*` reports for Rezonality depth/color attachments across several frames and a pane resize, and the robot correctly occluding itself against the sky.
  - macOS/Metal: the `rezonality-pbr-robot` and `rezonality-deferred-shading` render goldens pass unchanged in the Rezonality aggregate (Metal code is unchanged).
- [x] **Validation:** Preserve Metal behavior; run the Rezonality-scoped aggregate, Vulkan rendering checks, and same-cache smoke.
  - macOS (2026-10-08, with cards 04/06 on top): `python3 do.py test debug --rezonality` — all seven Rezonality render goldens (including `rezonality-pbr-robot` and `rezonality-deferred-shading`) and the Rezonality runtime/audio/layout/Neovim suites passed; the Rezonality contract and project suites failed only under concurrent multi-agent load (shared `/tmp` fixture paths, watcher timing) and passed when rerun in isolation. `python3 do.py smoke debug --skip-build` passed.
  - The Vulkan file was type-checked on macOS with `clang++ -fsyntax-only` against Vulkan 1.4 headers and VMA 3.1.0, but it is not compiled or run by the macOS build. Windows: build, run the Rezonality render goldens (`py do.py test debug --rezonality`) and the validation-layer check above.

## Windows evidence and blockers, 2026-10-08

Parent's `py do.py test debug --rezonality` retained `build-ninja-debug/windows-gates/final-core-rezonality-ctest.log`: all seven golden comparisons pass, but the real application emits depth load/store READ_AFTER_WRITE, stencil-transition WRITE_AFTER_WRITE, and cross-submit attachment hazards (for example PBR lines 1549 onward). This is not clean synchronization validation. Core + product selection was 64/70; paired fresh-profile same-cache Debug smoke passed. The separate stale NVIDIA `W:\p4\...VK_LAYER_NV_GPU_Trace...json` loader error must not be confused with or used to dismiss these genuine synchronization reports.

Source attribution, not instrumented handle identification: host `libs/draxul-renderer/src/vulkan/vk_context.cpp:330` has no source access mask, omits late fragment tests, and omits destination color/depth reads in its incoming dependency. Its depth attachment uses `STORE_OP_DONT_CARE` even for subsequent LOAD continuation passes; attachment lifetime semantics also need review. The renderer supplies that host load pass to plugins. NanoVG `libs/draxul-nanovg/backend/src/nanovg_vk.cpp:535` clears stencil from UNDEFINED while both external dependencies cover color stages/access only, matching the stencil hazard's reported access scopes. Rezonality's own offscreen dependencies already include early/late depth tests and attachment read/write access.

The new native PBR fixture uses a color-only host target (no real host depth continuation or NanoVG), exercises eight alternating resizes, and reported 421/422 assertions passing; its validation assertion failed only on the stale loader manifest, not SYNC hazards. This supports the host/NanoVG attribution, but is not a passing suite or proof that every packaged-plugin path is safe. It only loads `pbr_robot`; `robot2` has the same scenegraph but different fragment shaders, and deferred shading is not resized. Keep both acceptance and validation unchecked until actual packaged synchronization hazards are resolved and the required project/resize/occlusion evidence is retained. A separate ray-tracer scratch-buffer hazard is tracked in [15 ray-build-scratch-synchronization -bug.md](15%20ray-build-scratch-synchronization%20-bug.md).

## Host/NanoVG fixes and Windows acceptance — 2026-10-08

The remaining hazards were in core, as attributed above, and were fixed there:

- `libs/draxul-renderer/src/vulkan/vk_context.cpp` `create_render_pass()`: the
  external dependency now covers color output plus early **and late** fragment
  tests on both sides, with color/depth write source access and color/depth
  read+write destination access; depth `storeOp` is now `STORE` because LOAD
  continuation passes (host overlays and plugins) read it.
- `libs/draxul-nanovg/backend/src/nanovg_vk.cpp`: both external dependencies of
  the NanoVG pass (stencil cleared from `UNDEFINED`) now include the
  fragment-test stages and depth/stencil access.

Evidence, Windows/Vulkan Debug, SDK Khronos layer with sync, submit-time and
load-after-store validation, empty implicit-layer path:

| Scenario | Before host fixes | After |
|---|---|---|
| `rezonality-pbr-robot` | 32 SYNC hazards (RAW/WAW) | **0**, exit 0, 19.31 s |
| `rezonality-deferred-shading` | 40 SYNC hazards | **0**, exit 0, 6.34 s |
| `rezonality-ray-tracer` | 32 SYNC hazards | **0**, exit 0, 18.45 s |
| `examples/robot2` (scratch scenario) | — | **0**; capture shows correct self-occlusion against the sky |

Live session (`dgslep9mge`): `pbr_robot` and `robot2` side by side, three
native window resizes (swapchain/render-pass recreation) and two split
resizes: 0 validation errors and 0 SYNC hazards at all six checkpoints; client
and isolated server both exited 0. Screen captures were unavailable because
the desktop was in use (foreground not granted); occlusion evidence is the
offscreen `robot2` capture and the passing `pbr-robot` golden.

## Validation summary — Windows, 2026-10-08

- Core + products aggregate from the root (`py do.py test debug --products`,
  90 entries, 533 s): all seven Rezonality render goldens passed (including
  `rezonality-pbr-robot`, `rezonality-deferred-shading` and
  `rezonality-ray-tracer`). The Rezonality native shard failed only its
  validation-cleanliness check on the stale `W:\p4` implicit-layer loader
  manifest (421/422; the strict isolated sync-validation runs use an empty
  implicit-layer path and were clean). `draxul-rezonality-agent-layout` failed
  on a Windows cp1252 decode in `tests/rezonality_layout_integration.py`; that
  harness now decodes subprocess output as UTF-8 and the test passed (5.9 s).
- Same-cache Debug smoke passed; final Release startup passed (4.8 s), both
  through the root's isolated-runtime smoke.
- Metal is unchanged and relies on normal cross-platform CI.
