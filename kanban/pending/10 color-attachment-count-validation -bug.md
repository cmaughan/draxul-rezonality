# Reject unsupported color-output counts

**Summary:** Reject excessive color outputs so an invalid scene cannot crash the graphics backend.

**Priority:** 10  
**Severity:** CRITICAL  
**Source:** `plugins/rezonality/src/native_backend_metal.mm`

**Evidence and trigger:** B08; unchecked scene targets exceed Metal’s fixed slots, and Vulkan also lacks a device-limit check.

- [ ] **Investigate:** Trace target counting and validation before pipeline preparation and recording.
- [ ] **Fix:** Reject unsupported counts before native indexing, using each backend’s actual limit.
- [ ] **Acceptance:** A nine-color Metal candidate and Vulkan candidates exceeding the device limit fail with diagnostics while retaining the active scene.
- [ ] **Validation:** Check accepted boundary counts; run the Rezonality-scoped aggregate, both backend checks, and same-cache smoke.
