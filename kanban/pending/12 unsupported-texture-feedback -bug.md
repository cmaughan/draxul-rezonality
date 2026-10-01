# Reject unsupported previous-frame and aliased inputs

**Summary:** Reject unsupported feedback inputs so scenes cannot read a texture while writing the same output.

**Priority:** 12  
**Severity:** HIGH  
**Source:** `plugins/rezonality/src/live_project.cpp`

**Evidence and trigger:** B26; the parser accepts previous-frame sampling, but both backends resolve it to the current target texture.

- [ ] **Investigate:** Trace sampler flags and target identity through both native backends.
- [ ] **Fix:** Reject unsupported previous-frame inputs and same-pass target/sampler aliases before preparation.
- [ ] **Acceptance:** Previous-frame and plain aliased inputs fail with useful diagnostics while preserving the active scene; valid earlier-pass inputs remain accepted.
- [ ] **Validation:** Run the Rezonality-scoped aggregate, relevant checks on both backends, and same-cache smoke.

**Model:** `gpt-6.1-sol`
