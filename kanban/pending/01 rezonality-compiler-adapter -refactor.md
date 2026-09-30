# Isolate Rezonality compiler execution and diagnostics

**Priority:** P2 — production decoding is tied to real process execution.  
**Source:** `plugins/rezonality/src/live_project.cpp`  
**Proposed by:** Codex 6. **Owner:** one Rezonality project agent.  
**Evidence:** source combines OS process runner, diagnostic parser, compiler and watcher; existing injection supplies already-decoded diagnostics.

**Boundary verification**
- [ ] Record arguments, temporary outputs, limits, cleanup and compiler error formats.
**Implementation and migration**
- [ ] Add private typed process-result/compiler adapter within project target; retain pipeline and watcher APIs.
**Unit tests**
- [ ] Recorded spawn/exit/timeout/output/diagnostic cases; retain real edit-break-repair integration.
**Cross-platform validation**
- [ ] Check Windows quoting and POSIX argument boundaries, `--rezonality` aggregate, goldens and smoke.
**Agent documentation and tooling**
- [ ] Update product project/compiler boundary notes.
**Acceptance criteria**
- [ ] Production diagnostic decoding is testable without invoking a compiler.
