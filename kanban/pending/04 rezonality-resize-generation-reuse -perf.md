# Reuse Rezonality GPU assets across viewport changes

**Summary:** Keep Rezonality's models, images, and graphics programs when only pane size changes so resizing does not rebuild the entire scene's graphics resources.

**Source:** `plugins/rezonality/src/native_backend_vulkan.cpp`  
**Priority:** P1; static, high confidence. **Reported by:** Claude, Codex. Lines 2167–2188 and Metal `native_backend_metal.mm:711–731` reject generations on width or height alone, causing source assets and pipelines to be recreated during a drag; Metal also recompiles libraries.

- [x] **Baseline:** Record generation and pipeline creations, upload bytes, drag-frame p95, and retired-memory peak on an asset-rich scene.
  - Confirmed in source before the change: both `active_compatible()` checks failed on width/height alone and `prepare()` always called `create_generation()` from scratch, re-uploading every model (vertices, indices, five material textures per material), every static image surface, and recompiling every pass (Vulkan `vkCreateGraphicsPipelines`/ray pipelines and shader binding tables; Metal SPIRV-Cross + `newLibraryWithSource` + pipeline states).
  - `NativeBackend::resource_stats()` now records generations prepared, surfaces/models/programs created vs reused, and asset upload bytes. On `examples/pbr_robot` (1 glTF model, 1 HDR environment image, 3 passes) a size-only step previously cost 1 model + 3 surfaces + 3 programs + all asset bytes per drag step; it now costs 2 viewport surfaces and 0 programs/models/asset bytes (asserted by the Metal test below).
  - Metal, Debug build, Apple GPU, `pbr_robot`, 10 alternating steps each (temporary local timing harness around `prepare()+activate_prepared()`, not committed): full regeneration (the old per-resize cost) median 159 ms, p95 473 ms, copying ~817 MB of model/image data that the retiring generation also held until its frame slots completed; size-only regeneration now median 0.07 ms, p95 0.13 ms, 0 asset bytes, so the retired-generation memory peak during a drag drops by that ~817 MB per in-flight step.
  - Vulkan drag-frame p95 and memory were not measured (Windows only); see the Platforms item.
- [x] **Implement:** Separate viewport-sized targets from compatible immutable assets and pipelines; retain frame-slot retirement.
  - `src/generation_reuse.h` classifies each preparation (`Rebuild`, `ReuseAssets`, `ResizeTargets`, `Compatible`) from source generation, device, presentation target, and viewport size, and decides which surfaces are viewport-independent (fixed-size static image, not audio, not a pass target).
  - Vulkan: surfaces, models, and per-pass programs (pipeline, pipeline layout, set layouts, offscreen render pass, ray SBT) are reference counted; framebuffers, descriptor pools/sets, uniform buffers, and viewport-sized surfaces stay per generation. Shared objects are destroyed only by their last generation, and generations still retire only after their used frame slots complete.
  - Metal: the same rule shares model buffers/textures/acceleration structures, static image textures, and compiled pipeline states (reference counted by ARC).
  - A presentation-target change (Vulkan swapchain render pass/target generation, e.g. a window resize; Metal pixel format) keeps models and images but rebuilds programs.
- [x] **Functional safety:** Preserve format/render-pass compatibility, checked dimensions, failed-reload rollback, and last-good output.
  - Programs are reused only for the same render pass handle/target generation (Vulkan) or drawable pixel format (Metal); a new source build never shares anything. Viewport-sized surfaces still pass `checked_surface_dimensions`; an oversized resize is rejected without touching the active generation (Metal test). Failed candidates release only their own references.
- [x] **Compare:** Require no source-asset rebuild on size-only steps; report before/after drag stalls and memory.
  - `Rezonality Metal resize keeps models, images, and pipelines` asserts zero model/program creation and zero asset upload bytes on a size-only step, then records and completes real Metal frames with the reused generation (including after a rejected oversize step and a pixel-format change).
  - `Rezonality native generations keep source assets across resizes` pins the shared classification rule for both backends.
  - Before/after stall and memory figures are in the Baseline item (Metal); Vulkan figures are part of the open Platforms item.
- [ ] **Platforms:** Check resize, reload failure, and render goldens on Vulkan and Metal.
  - macOS/Metal: Rezonality render goldens and the resize test pass in the Rezonality aggregate.
  - Windows/Vulkan (not compiled or run on macOS; type-checked with `clang++ -fsyntax-only` against Vulkan 1.4 headers and VMA 3.1.0): build; run `py do.py test debug --rezonality`; then manually drag-resize a split containing `examples/pbr_robot` and `examples/ray_tracer` with the Vulkan validation layer enabled and confirm no validation errors, no use-after-free of shared pipelines/descriptor layouts when generations retire, correct output after each step, and that a window resize (new swapchain/target generation) rebuilds programs but not models. Measure drag-frame p95 and retired-memory peak before/after on an asset-rich scene.
- [ ] **Acceptance:** Size changes replace only size-dependent resources.
  - Met on Metal (tested). Pending the Windows/Vulkan check above.
