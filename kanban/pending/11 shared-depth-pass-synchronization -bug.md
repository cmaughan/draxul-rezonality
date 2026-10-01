# Synchronize reused depth images

**Summary:** Synchronize shared depth images so successive drawing passes preserve correct visibility.

**Priority:** 11  
**Severity:** HIGH  
**Source:** `plugins/rezonality/src/native_backend_vulkan.cpp`

**Evidence and trigger:** B25; the shipped robot scene reuses depth across clear/load/test/write passes, but dependencies cover only color and sampling.

- [ ] **Investigate:** Trace depth initialization, pass dependencies, loads/stores, and reuse across frames.
- [ ] **Fix:** Add the necessary depth stages and read/write dependencies without weakening color synchronization.
- [ ] **Acceptance:** The robot scene and another shared-depth case pass synchronization validation and preserve expected occlusion.
- [ ] **Validation:** Preserve Metal behavior; run the Rezonality-scoped aggregate, Vulkan rendering checks, and same-cache smoke.
