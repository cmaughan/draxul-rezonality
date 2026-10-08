# Synchronize reused ray acceleration-structure scratch memory

**Summary:** Order BLAS and TLAS builds that reuse one scratch buffer so the ray-tracer project is synchronization-safe.

**Priority:** P1 — overlapping acceleration-structure scratch writes violate Vulkan synchronization.

Evidence: Windows 2026-10-08 `draxul-render-rezonality-ray-tracer` golden comparison passed, but `build-ninja-debug/windows-gates/final-core-rezonality-ctest.log` records `SYNC-HAZARD-WRITE-AFTER-WRITE` at `vkCmdBuildAccelerationStructuresKHR` on the reused scratch buffer (offset 0, size 2176). The reported barrier allows acceleration-structure READ, not the required WRITE. This is distinct from the stale NVIDIA layer manifest and host depth/NanoVG hazards tracked in [11 shared-depth-pass-synchronization -bug.md](11%20shared-depth-pass-synchronization%20-bug.md).

- [x] Inspect `src/native_backend_vulkan.cpp:2315`: the BLAS-to-TLAS barrier targets acceleration-structure READ only; the second build retains the same scratch device address.
- [ ] Correct the dependency for reused scratch writes while preserving BLAS-to-TLAS reads; inspect the corresponding Metal path without inventing a Vulkan requirement there.
- [ ] Run the ray-tracer scenario with synchronization and submit-time validation enabled; require no scratch hazards, not merely a matching image.
- [ ] Run the Rezonality aggregate and same-cache smoke; retain output, failures, and timing.

No production fix or new GPU execution was performed during this audit. Parent owns the shared build cache.
