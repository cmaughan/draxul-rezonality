# Avoid rereading unchanged watched projects

**Source:** `plugins/rezonality/src/live_project.cpp`  
**Priority/evidence:** P1; static, high confidence. **Reported by:** Claude, Codex. `ProjectPipeline::fingerprint()` at lines 1238–1277 enumerates, sorts, reads, and hashes every file; lines 1544–1555 repeat after 100 ms even without edits. Its worker protects GUI latency, but idle I/O, CPU, and power scale with project bytes and pane count.

- [ ] **Baseline:** Measure unchanged bytes read, worker CPU, and edit-to-rebuild latency for one and several panes on an asset-rich project.
- [ ] **Implement:** Use native invalidation or a bounded per-file index, with overflow and missed-event rescans.
- [ ] **Functional safety:** Detect same-size edits, arbitrary-suffix shader includes, break/repair, and failed scans; preserve last-good builds.
- [ ] **Compare:** Require no repeated full-content reads while unchanged and bounded edit latency.
- [ ] **Platforms:** Verify native watcher behavior and Rezonality output on Windows/Vulkan and macOS/Metal.
- [ ] **Acceptance:** Idle project contents are not reread every poll.
