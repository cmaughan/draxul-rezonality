# Models, cameras, and PBR

- [x] Load OBJ, glTF, and GLB geometry through a pinned, plugin-owned Assimp.
- [x] Publish immutable CPU model, material, and texture candidates from the
      live-project worker.
- [x] Upload model vertex/index buffers and PBR descriptor resources on Vulkan.
- [x] Implement the paired Metal model buffers, texture arrays, and indexed draws.
- [x] Populate model/view/projection camera uniforms and route left-drag orbit,
      wheel dolly, pause, viewport, and DPI input through the plugin ABI.
- [x] Treat missing referenced model textures as candidate failures so the last
      successful generation remains intact.
- [x] Restore the sphere geometry in the default and protoplanetary projects.
- [x] Stage the preserved PBR robot, material textures, HDR environment, and
      asset license.
- [x] Add model/texture immutability coverage and deterministic Windows Vulkan
      snapshots for the sphere and PBR robot.
- [x] Build and bless the corresponding Metal snapshots on macOS.
- [ ] Perform the two-pane orbit/edit/continuous-resize manual check on both
      platforms.

## macOS validation evidence

- 2026-09-23: blessed and visually inspected the sphere-bearing default and
  protoplanetary scenes plus the PBR robot Metal reference on Apple M5.
- The Release aggregate passed their registered comparisons together with the
  pane-local camera/resize, dynamic PBR edit, and missing-asset rollback tests.
- The two-pane continuous orbit/edit/resize observation remains a manual gate.
