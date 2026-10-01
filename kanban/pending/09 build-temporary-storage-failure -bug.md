# Contain temporary-storage failures during rebuilding

**Summary:** Report unavailable temporary storage so a failed shader edit leaves the application running.

**Priority:** 09  
**Severity:** CRITICAL  
**Source:** `plugins/rezonality/src/live_project.cpp`

**Evidence and trigger:** B07; throwing temporary-directory lookup precedes candidate handling and escapes the unguarded worker build call.

- [ ] **Investigate:** Trace all operations outside existing build handlers and worker exception containment.
- [ ] **Fix:** Use error-code lookup and convert complete-build exceptions into ordinary generation diagnostics.
- [ ] **Acceptance:** Missing temporary storage preserves the active scene, publishes an error, and permits a later successful rebuild.
- [ ] **Validation:** Run the Rezonality-scoped aggregate, relevant reload checks, and same-cache smoke.
