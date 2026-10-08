# Synchronize reused ray acceleration-structure scratch memory

**Summary:** Order BLAS and TLAS builds that reuse one scratch buffer so the ray-tracer project is synchronization-safe.

**Priority:** P1 — overlapping acceleration-structure scratch writes violate Vulkan synchronization.

Evidence: Windows 2026-10-08 `draxul-render-rezonality-ray-tracer` golden comparison passed, but `build-ninja-debug/windows-gates/final-core-rezonality-ctest.log` records `SYNC-HAZARD-WRITE-AFTER-WRITE` at `vkCmdBuildAccelerationStructuresKHR` on the reused scratch buffer (offset 0, size 2176). The reported barrier allows acceleration-structure READ, not the required WRITE. This is distinct from the stale NVIDIA layer manifest and host depth/NanoVG hazards tracked in [11 shared-depth-pass-synchronization -bug.md](11%20shared-depth-pass-synchronization%20-bug.md).

- [x] Inspect `src/native_backend_vulkan.cpp:2315`: the BLAS-to-TLAS barrier targets acceleration-structure READ only; the second build retains the same scratch device address.
- [x] Correct the dependency for reused scratch writes while preserving BLAS-to-TLAS reads; inspect the corresponding Metal path without inventing a Vulkan requirement there.
- [x] Run the ray-tracer scenario with synchronization and submit-time validation enabled; require no scratch hazards, not merely a matching image.
- [x] Run the Rezonality aggregate and same-cache smoke; retain output, failures, and timing.

No production fix or new GPU execution was performed during this audit. Parent owns the shared build cache.

## Fix and validation — Windows, 2026-10-08

- `build_model_acceleration_structures()` (`src/native_backend_vulkan.cpp`): the
  BLAS-to-TLAS memory barrier now uses acceleration-structure READ|WRITE for both
  source and destination access. The spec treats build scratch accesses as
  acceleration-structure read/write at the build stage, so this orders the TLAS
  build's reuse of the scratch address after the BLAS build's writes while
  keeping the BLAS read for the TLAS build. The final TLAS-to-ray-tracing barrier
  is unchanged.
- Metal inspected, no change: `acceleration_scratch` is a default (tracked)
  `MTLResourceStorageModePrivate` buffer, and the BLAS and TLAS builds use
  separate acceleration-structure encoders on one command buffer, so Metal's
  hazard tracking already orders them.
- `draxul-render-rezonality-ray-tracer` scenario run directly under the SDK
  Khronos layer with `VK_KHRONOS_VALIDATION_VALIDATE_SYNC`,
  `..._SYNCVAL_SUBMIT_TIME_VALIDATION` and
  `..._SYNCVAL_LOAD_OP_AFTER_STORE_OP_VALIDATION`, empty implicit-layer path:
  **0** `SYNC-HAZARD` reports, 0 scratch (`vkCmdBuildAccelerationStructuresKHR`)
  hazards, 0 validation errors, exit 0, 18.45 s. The first run in the same
  environment, before the host fixes recorded on card 11, still reported 32
  host attachment hazards but already **0 scratch hazards**, which confirms both
  that sync validation was active and that this barrier was the scratch fix.
  Logs: `build-ninja-debug/windows-gates/rezo-sync-20261008{,-b}/` (root tree).

## Validation summary — Windows, 2026-10-08

- Core + products aggregate from the root (`py do.py test debug --products`,
  90 entries, 533 s): all seven Rezonality render goldens passed (including
  `rezonality-pbr-robot`, `rezonality-deferred-shading` and
  `rezonality-ray-tracer`). The Rezonality native shard failed only its
  validation-cleanliness check on the stale `W:\p4` implicit-layer loader
  manifest (421/422; the strict isolated sync-validation runs use an empty
  implicit-layer path and were clean). `draxul-rezonality-agent-layout` failed
  on a Windows cp1252 decode in `tests/rezonality_layout_integration.py`; that
  harness now decodes subprocess output as UTF-8 and the test passed (5.9 s).
- Same-cache Debug smoke passed; final Release startup passed (4.8 s), both
  through the root's isolated-runtime smoke.
- Metal is unchanged and relies on normal cross-platform CI.
