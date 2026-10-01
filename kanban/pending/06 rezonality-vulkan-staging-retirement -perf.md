# Retire one-time Rezonality Vulkan texture staging

**Summary:** Release temporary texture-upload memory after the graphics card has finished using it so Rezonality on Windows does not retain unnecessary copies of static images.

**Source:** `plugins/rezonality/src/native_backend_vulkan.cpp`  
**Priority/evidence:** P2; static, high confidence. **Reported by:** Claude, Codex. Lines 556–559 and 634–638 retain mapped staging buffers after one-time texture upload at 1875–1881 and 1934–1973; lines 2206–2223 retire whole generations only. A 4096² RGBA buffer represents 64 MiB of staging by arithmetic, not an observed allocation.

- [ ] **Baseline:** Measure static staging bytes after initial frames, after reload, and at generation retirement.
- [ ] **Implement:** Retire completed one-time upload buffers with their submitting frame slot; retain streaming audio buffers.
- [ ] **Functional safety:** Cover aborted recording, failed submit, in-flight completion, and visible texture integrity.
- [ ] **Compare:** Require static staging to return to steady baseline after upload completion.
- [ ] **Platforms:** Run Windows/Vulkan validation and Rezonality renders; inspect Metal’s separate upload policy.
- [ ] **Acceptance:** Uploaded static textures do not retain their one-time mapped staging.
