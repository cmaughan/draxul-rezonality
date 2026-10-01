# Reuse decoded Rezonality assets across shader-only edits

**Summary:** Reuse unchanged models and images when editing shaders so Rezonality reloads do not repeatedly import the same assets.

**Source:** `plugins/rezonality/src/live_project.cpp`  
**Priority/evidence:** P2; static, medium-high confidence. **Reported by:** Claude. `build_candidate()` at lines 1316–1326 reloads models and lines 1357–1385 decode images for every candidate, including a `.frag` edit. The watcher detects the edit, but no decoded-asset cache separates unchanged inputs.

- [ ] **Baseline:** Count import/decode calls, reload latency, and peak memory for repeated shader edits in an asset-rich project.
- [ ] **Implement:** Cache bounded immutable decoded assets by source/options and complete external dependencies.
- [ ] **Functional safety:** Detect same-size edits and model-referenced files; preserve break/repair and last-good generations.
- [ ] **Compare:** Require zero unchanged model/image decodes for shader-only edits without unbounded cache growth.
- [ ] **Platforms:** Check reloads on Vulkan and Metal, aggregate and smoke.
- [ ] **Acceptance:** Shader edits rebuild shader work without reimporting unchanged assets.
