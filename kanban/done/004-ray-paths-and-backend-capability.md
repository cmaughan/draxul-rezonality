# Ray paths and backend capability handling

- [x] Stage the preserved VkLive Cornell-box ray project and native shaders.
- [x] Parse, watch, and compile Vulkan ray groups without weakening raster
      project validation.
- [x] Enable optional Vulkan ray-tracing device capabilities only when the
      selected device supports the complete required feature/extension set.
- [x] Build plugin-owned BLAS/TLAS, shader-binding tables, descriptors, and ray
      pipelines while recording only into Draxul's borrowed command buffer.
- [x] Build Metal acceleration structures and dispatch the preserved native
      Metal ray kernel through the borrowed command buffer.
- [x] Report a deterministic unsupported-device status while preserving the
      pane and any last valid active generation.
- [x] Prove invalid ray shaders and missing geometry roll back, then recover
      after repair through the real dynamically loaded module.
- [x] Add a conditional Windows Vulkan Cornell-box render snapshot.
- [x] Build and bless the corresponding macOS Metal render snapshot.
- [x] Perform the break/repair and resize manual check on supported Metal hardware.
- [x] Perform the break/repair and resize manual check on supported Vulkan hardware.

## macOS validation evidence

- 2026-09-23: blessed and visually inspected the Cornell-box Metal reference on
  Apple M5; its registered Release comparison passed.
- The same aggregate passed dynamic invalid-shader and missing-geometry rollback
  and repair coverage. The hands-on break/repair/resize observation remains open
  on supported Vulkan and Metal hardware.

## 2026-09-23 pending-lane verification

- The Debug aggregate passed the Cornell-box Metal comparison and the dynamic
  invalid-shader/missing-geometry rollback and repair coverage.
- A later isolated live session loaded the real Metal ray project, introduced an
  invalid Metal shader at generation 2, and observed generation 1 remain active
  while six split ratios were exercised. After restoring the shader, generation
  3 activated while five more ratios were exercised and the UI stayed attached.
  This completes the Metal gate; supported Vulkan hardware remains open.

## Windows Vulkan validation evidence

- The registered Vulkan Cornell-box comparison passed in the 38-test Debug
  aggregate, establishing that this device exposes the required ray features.
- In an isolated live session, an invalid token in `rt_gen.rgen` produced a
  generation-2 compile error at line 5 while generation 1 remained active.
  Six split ratios were exercised without losing the pane or last good image.
- Repairing the shader promoted generation 3, cleared the diagnostic to
  `active generation ready`, and restored the visible ray-traced Cornell box.
