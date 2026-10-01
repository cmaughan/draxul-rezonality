# Reuse module implementation and partition contract tests

**Summary:** Reuse compiled Rezonality code and separate its test groups so tests avoid rebuilding the same implementation or depending on unrelated parts of the plugin.

**Priority:** P2 — the contract target recompiles module/native sources and mixes test kinds.  
**Source:** `plugins/rezonality/cmake/Tests.cmake`  
**Proposed by:** Claude 45, narrowed. **Owner:** one Rezonality build/test agent.  
**Evidence:** test target recompiles plugin and selected backend, depends on the module, and runs serial; pure, in-process and dynamic-load cases share one suite.

**Boundary verification**
- [ ] Classify export, dynamic-load, edit and pure cases plus module/test link flags.
**Implementation and migration**
- [ ] Share private implementation objects between MODULE and in-process test; move pure cases to existing project/runtime/audio targets and narrow include ownership.
**Unit tests**
- [ ] Preserve ABI export and case inventory; keep only required dynamic tests serial.
**Cross-platform validation**
- [ ] Preserve host-resolved module SDL versus test-owned SDL and paired Vulkan/Metal sources; run aggregate, goldens and smoke.
**Agent documentation and tooling**
- [ ] Update product target map.
**Acceptance criteria**
- [ ] Native implementation compiles once per configuration and dynamic loading still works.
