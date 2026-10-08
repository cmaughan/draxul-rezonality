# Reuse module implementation and partition contract tests

**Summary:** Reuse compiled Rezonality code and separate its test groups so tests avoid rebuilding the same implementation or depending on unrelated parts of the plugin.

**Priority:** P2 — the contract target recompiles module/native sources and mixes test kinds.  
**Source:** `plugins/rezonality/cmake/Tests.cmake`  
**Proposed by:** Claude 45, narrowed. **Owner:** one Rezonality build/test agent.  
**Evidence:** test target recompiles plugin and selected backend, depends on the module, and runs serial; pure, in-process and dynamic-load cases share one suite.

**Boundary verification**
- [x] Classify export, dynamic-load, edit and pure cases plus module/test link flags.
  - Confirmed: `draxul-test-rezonality` recompiled `rezonality_plugin.cpp` and the selected backend, depended on the module, and ran all 15 contract cases plus the Metal case serially.
  - Export (in-process ABI): "exports a usable Draxul plugin contract". Edit (in-process live reload): "watches valid, broken, and repaired shader edits", "compiles every staged example". Native device: Metal color-target limits. Dynamic load: the four "The staged Rezonality module ..." cases. Pure: GPU rollback transaction and Metal depth-state source check (backend-neutral policy); diagnostics publication (2), NYX example content, model loading (2), and camera (project library).
  - Link flags: the module keeps `-undefined dynamic_lookup` for host SDL plus the exported-symbol list; executables own `SDL3::SDL3`.
**Implementation and migration**
- [x] Share private implementation objects between MODULE and in-process test; move pure cases to existing project/runtime/audio targets and narrow include ownership.
  - New OBJECT library `draxul-rezonality-native` (ABI adapter plus selected backend) carries the native usage requirements; the MODULE and the new `draxul-test-rezonality-native` link the same objects. An object library (not a static archive) keeps the otherwise unreferenced exported entry point in the module.
  - Pure cases moved to new sources in existing targets: `tests/rezonality_project_support_tests.cpp` (project) and `tests/rezonality_backend_policy_tests.cpp` (runtime). No audio case was misplaced. In-process cases are in `tests/rezonality_native_contract_tests.cpp`; `tests/rezonality_plugin_contract_tests.cpp` keeps the dynamic cases in place and includes no product headers. Shared host fakes live in `tests/rezonality_plugin_test_support.h`.
**Unit tests**
- [x] Preserve ABI export and case inventory; keep only required dynamic tests serial.
  - Catch inventory unchanged: 41 cases with identical names/tags (before: rezonality 16, project 12, runtime 7, audio 6; after: rezonality 4, native 4, project 18, runtime 9, audio 6). Root static audit unchanged at 1910 registrations / 338 tags (sources 173 -> 176).
  - CTest: 81 -> 82 entries, `scope-rezonality` 13 -> 14 (adds `draxul-test-rezonality-native-shard-0`). Only `draxul-test-rezonality-shard-0` (dynamic) remains `RUN_SERIAL`, besides the pre-existing agent-layout test. The module still exports only `_draxul_plugin_query_v2`.
**Cross-platform validation**
- [x] Preserve host-resolved module SDL versus test-owned SDL and paired Vulkan/Metal sources; run aggregate, goldens and smoke.
  - Backend source is still selected per platform into the one object library; Vulkan/VMA/VulkanResources and spirv-cross/Metal links moved with it. The dynamic loader force-loads static SDL on macOS (as `draxul-test-plugin-integration` does) so `RTLD_NOW` binds the module's SDL calls; Windows modules link SDL themselves.
  - macOS: `do.py test debug --rezonality` built and passed every Rezonality entry including all seven render goldens; the same-cache smoke passed. Windows configuration was not built locally and is left to routine CI.
**Agent documentation and tooling**
- [x] Update product target map.
  - `AGENTS.md` target/test map and placement rule; `README.md` product boundary; comments in `cmake/Tests.cmake`.
**Acceptance criteria**
- [x] Native implementation compiles once per configuration and dynamic loading still works.
  - Only `draxul-rezonality-native.dir` compiles `rezonality_plugin.cpp` and `native_backend_metal.mm`; both the module and native test link those objects, and the dynamic suite passed against the built module.
