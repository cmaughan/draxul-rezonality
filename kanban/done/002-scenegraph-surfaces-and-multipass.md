# Scenegraph surfaces and multipass rendering

- [x] Parse ordered passes, named color/float/depth surfaces, samplers, scales,
      clears, disabled passes, and screen geometry from VkLive scenegraphs.
- [x] Compile a complete immutable multi-pass candidate before publishing it.
- [x] Create pane-sized Vulkan and Metal intermediate targets transactionally.
- [x] Bind common uniforms, sampled surfaces, MRT/depth attachments, and final
      continuation composition.
- [x] Load RGBA texture assets off the render thread and upload them through
      plugin-owned resources without submitting Draxul's borrowed command buffer.
- [x] Retire resized/reloaded Vulkan generations by completed frame slot.
- [x] Stage adapted Slice 2 projects plus preserved original VkLive material.
- [x] Add dynamic-module compilation coverage and four Windows render snapshots.
- [x] Validate and bless the four Metal snapshots on macOS.
- [ ] Perform the manual continuous split-resize/edit check on Windows and macOS.

## macOS validation evidence

- 2026-09-23: blessed and visually inspected the `rezonality-plugin`,
  `rezonality-blend-waves`, `rezonality-deferred-shading`, and
  `rezonality-protoplanetary-disc` Metal references on Apple M5.
- The Release Rezonality aggregate passed all 37 selected tests, including the
  four registered Metal comparisons, and the same-cache Release smoke passed.
- Automated resize, edit, rollback, and recovery coverage passed. The continuous
  hands-on split-resize/edit observation remains open on both platforms.
