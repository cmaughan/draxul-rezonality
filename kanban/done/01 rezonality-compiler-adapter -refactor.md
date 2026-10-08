# Isolate Rezonality compiler execution and diagnostics

**Summary:** Separate running the shader compiler from interpreting its output so Rezonality's error messages and failure handling can be tested without launching a real compiler.

**Priority:** P2 — production decoding is tied to real process execution.  
**Source:** `plugins/rezonality/src/live_project.cpp`  
**Proposed by:** Codex 6. **Owner:** one Rezonality project agent.  
**Evidence:** source combines OS process runner, diagnostic parser, compiler and watcher; existing injection supplies already-decoded diagnostics.

**Boundary verification**
- [x] Record arguments, temporary outputs, limits, cleanup and compiler error formats. Arguments: `-V --target-env vulkan1.2 <shader> -o <output> -l -g -I<project>`, plus `-DREZONALITY_METAL_SEPARATE_MODEL_SAMPLER=1` on macOS. Windows uses the project-relative form (card 14). Temporary outputs: `<temp>/draxul-rezonality/<pipeline>/{vertex,fragment,raygen,miss,closest}-<generation>-<pass>.spv`, named and removed by `build_candidate` after each pass. Limits: 10 s timeout, then kill; 128 diagnostics per candidate; 300-character fallback message; captured output is now capped at 1 MiB and the rest is still drained. Recorded glslangValidator formats: `ERROR|WARNING: <path>:<line>: <message>`, a leading file-name echo line, the `N compilation errors` summary, path-less `Linking <stage> stage` errors, `Error: unable to open input file` for a missing input, and `Failed to open file` with exit code 0 when the output cannot be written.
**Implementation and migration**
- [x] Add private typed process-result/compiler adapter within project target; retain pipeline and watcher APIs. `src/shader_compiler.{h,cpp}` in `draxul-rezonality-project` contains `ProcessRequest`/`ProcessResult` (`Exited`/`StartFailed`/`TimedOut`), the replaceable `ProcessRunner`, `plan_compiler_invocation`, `parse_compiler_diagnostics`, and `compile_shader(request, CompilerEnvironment, ...)`. `ProjectPipeline`, `LiveProject`, `build_candidate`, and `CompileShaderOperation` are unchanged. A compiler exit of 0 with no SPIR-V now keeps glslang's explanation, and names that cannot be decoded fall back to the shader path instead of throwing.
**Unit tests**
- [x] Recorded spawn/exit/timeout/output/diagnostic cases; retain real edit-break-repair integration. `Rezonality compiler adapter interprets recorded process outcomes` covers spawn failure, timeout, relative and absolute diagnostics, warnings, link and usage fallbacks, empty output, the diagnostic cap, bytes that cannot be decoded, reading the SPIR-V output, and exit 0 without output. Real-compiler coverage is `Rezonality bundled compiler builds international projects` plus the existing real-module edit/break/repair tests.
**Cross-platform validation**
- [x] Check Windows quoting and POSIX argument boundaries. `Rezonality quotes Windows compiler arguments for the CRT` runs on every platform. Arguments that contain spaces and emoji reach the real macOS compiler as single argv entries.
- [x] macOS `--rezonality` aggregate (registered Rezonality goldens included) and same-cache smoke.
- [x] Windows aggregate, goldens, and same-cache smoke executed on 2026-10-08; compiler-specific coverage passed. Scope failures and costs are retained below.
**Agent documentation and tooling**
- [x] Update product project/compiler boundary notes (`AGENTS.md`).
**Acceptance criteria**
- [x] Production diagnostic decoding is testable without invoking a compiler.

## Windows closure evidence, 2026-10-08

Parent ran `py do.py test debug --rezonality` in `build-ninja-debug` after rebuilding the explicit-UCN international fixtures: project suite passed (13.30 s), dynamic module passed (262 assertions / 5 cases, 85.53 s), Neovim passed (4.27 s), and all seven golden comparisons passed (114.19 test-seconds). Full selection: 64/70 passed, CTest 306.89 s / runner 308.27 s; 19 build steps in 130.635 s, no reconfigure. Five core entries failed; the native Vulkan suite also failed its validation-log assertion on a stale machine NVIDIA layer manifest (421/422 assertions passed). Golden logs contain real synchronization hazards, so comparison success is not synchronization-clean evidence: [shared-depth tracking](11%20shared-depth-pass-synchronization%20-bug.md) and [ray scratch tracking](15%20ray-build-scratch-synchronization%20-bug.md) remain open.

Paired fresh-profile same-cache Debug smoke passed, exit 0 (35.273 s wrapper total including MSVC setup, parent-measured); the earlier default-profile timeout remains unresolved in the parent's separate startup investigation. Retained parent logs: `build-ninja-debug/windows-gates/final-core-rezonality.log`, `final-core-rezonality-ctest.log`, and `final-debug-smoke.log`. This closes the compiler slice, not the aggregate's unrelated failures; no new macOS run is claimed.

Final Release confirmation (parent, 2026-10-08): same-cache smoke retry passed, exit 0, 33.866647 s including toolchain setup; log `build-ninja-debug/windows-gates/final-release-smoke-retry.log`. The first attempt failed during resource-exhausted MSVC environment setup (68.059 s), before app startup; it is retained as an environmental failure, not an application failure.
