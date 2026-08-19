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
- [ ] Build and bless the corresponding macOS Metal render snapshot.
- [ ] Perform the break/repair and resize manual check on supported Vulkan and
      Metal hardware.
