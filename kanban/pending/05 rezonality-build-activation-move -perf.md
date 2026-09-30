# Avoid deep-copying Rezonality builds at activation

**Source:** `plugins/rezonality/src/runtime_controller.cpp`  
**Priority/evidence:** P2; static, high confidence. **Reported by:** Claude, Codex. Lines 47–62 copy `ShaderBuild`, including vectors of shaders, models, and pixels, before replacing active storage. The same copy also occurs for active-build resize recovery.

- [ ] **Baseline:** Count copied bytes, allocations, and peak memory during an asset-heavy reload and resize.
- [ ] **Implement:** Move a successfully prepared pending build; retain the owned active build for compatible recreation.
- [ ] **Functional safety:** Keep pending-rejection rollback, diagnostics/status, generation identity, and last-good output.
- [ ] **Compare:** Require no whole-build activation copy and report peak memory before/after.
- [ ] **Platforms:** Check Vulkan and Metal reload/resize paths, aggregate and smoke.
- [ ] **Acceptance:** Activation transfers ownership without duplicating immutable asset payloads.
