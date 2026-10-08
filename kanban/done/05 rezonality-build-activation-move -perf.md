# Avoid deep-copying Rezonality builds at activation

**Summary:** Transfer a finished Rezonality build into use without copying all its assets so scene reloads and resize recovery use less temporary memory.

**Source:** `plugins/rezonality/src/runtime_controller.cpp`  
**Priority:** P2; static, high confidence. **Reported by:** Claude, Codex. Lines 47–62 copy `ShaderBuild`, including vectors of shaders, models, and pixels, before replacing active storage. The same copy also occurs for active-build resize recovery.

- [x] **Baseline:** Count copied bytes, allocations, and peak memory during an asset-heavy reload and resize.
  - At a3ec63f, `RuntimeController::activate_prepared()` copied the
    selected `ShaderBuild` (every SPIR-V vector, model vertex/index/texture
    vector, and surface pixel vector) on each activation and on each
    active-build resize recreation. For `examples/pbr_robot` that is about
    817 MB of decoded payload copied per activation or resize. Peak memory
    during a reload reached old active + pending + copy (about 3x one
    build's payload).
- [x] **Implement:** Move a successfully prepared pending build; retain the owned active build for compatible recreation.
  - A pending build is moved into `active_build_`. Recreating the active
    build after a resize leaves it in place. Backends finish consuming the
    build in `prepare()` before activation, so no native object aliases the
    moved-from storage.
- [x] **Functional safety:** Keep pending-rejection rollback, diagnostics/status, generation identity, and last-good output.
  - Rejection still resets only the pending build. Status text, pass and
    surface counts, and active generation are taken from the activated
    build, and the existing runtime tests pass unchanged.
- [x] **Compare:** Require no whole-build activation copy and report peak memory before/after.
  - `Rezonality runtime transfers build payloads without copying` follows
    surface pixel, model vertex, and model texture storage from accepted
    candidate to activation, resize recreation, rejected replacement, and
    repaired activation. The same heap buffers stay active throughout, so
    0 bytes are copied, where previously the whole payload was. Peak memory
    during activation drops from about 3x to 2x one build's payload (old
    active retiring + new), and resize recreation no longer allocates a
    second copy.
- [x] **Platforms:** Check Vulkan and Metal reload/resize paths, aggregate and smoke.
  - The change is in the backend-neutral controller that both renderers
    use. macOS/Metal aggregate (contract, render goldens) and smoke pass
    locally. Windows/Vulkan is covered by routine CI.
- [x] **Acceptance:** Activation transfers ownership without duplicating immutable asset payloads.
