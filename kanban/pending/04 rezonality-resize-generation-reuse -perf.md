# Reuse Rezonality GPU assets across viewport changes

**Summary:** Keep Rezonality's models, images, and graphics programs when only pane size changes so resizing does not rebuild the entire scene's graphics resources.

**Source:** `plugins/rezonality/src/native_backend_vulkan.cpp`  
**Priority/evidence:** P1; static, high confidence. **Reported by:** Claude, Codex. Lines 2167–2188 and Metal `native_backend_metal.mm:711–731` reject generations on width or height alone, causing source assets and pipelines to be recreated during a drag; Metal also recompiles libraries.

- [ ] **Baseline:** Record generation and pipeline creations, upload bytes, drag-frame p95, and retired-memory peak on an asset-rich scene.
- [ ] **Implement:** Separate viewport-sized targets from compatible immutable assets and pipelines; retain frame-slot retirement.
- [ ] **Functional safety:** Preserve format/render-pass compatibility, checked dimensions, failed-reload rollback, and last-good output.
- [ ] **Compare:** Require no source-asset rebuild on size-only steps; report before/after drag stalls and memory.
- [ ] **Platforms:** Check resize, reload failure, and render goldens on Vulkan and Metal.
- [ ] **Acceptance:** Size changes replace only size-dependent resources.
