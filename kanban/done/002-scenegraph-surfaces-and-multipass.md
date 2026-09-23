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
- [x] Perform the manual continuous split-resize/edit check on macOS.
- [x] Perform the manual continuous split-resize/edit check on Windows.

## macOS validation evidence

- 2026-09-23: blessed and visually inspected the `rezonality-plugin`,
  `rezonality-blend-waves`, `rezonality-deferred-shading`, and
  `rezonality-protoplanetary-disc` Metal references on Apple M5.
- The Release Rezonality aggregate passed all 37 selected tests, including the
  four registered Metal comparisons, and the same-cache Release smoke passed.
- Automated resize, edit, rollback, and recovery coverage passed. The continuous
  hands-on split-resize/edit observation remains open on Windows.

## 2026-09-23 pending-lane verification

- The Debug Rezonality aggregate passed the module, project/runtime/audio,
  agent-layout, Neovim, and registered Metal render checks for this slice.
- A later isolated live session created two real deferred-shading panes from a
  copied project, exercised eleven split ratios from 0.20 through 0.80 while
  making a valid fragment-shader edit, and observed both panes advance from
  generation 1 to generation 2. The UI remained attached and returned to a
  0.50 split. This completes the macOS gate; Windows remains open.

## Windows validation evidence

- An isolated Debug Draxul session opened two real Vulkan deferred-shading
  panes from one copied project. Ten continuous split-ratio changes from 0.20
  through 0.80 remained responsive while a valid `lighting.frag` edit rebuilt
  both panes from generation 1 to generation 2.
- The multipass gradient remained visible after the live edit and final resize;
  both diagnostic records reported the same active generation without errors.
