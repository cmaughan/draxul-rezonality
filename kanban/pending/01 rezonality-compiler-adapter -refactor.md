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
- [ ] Windows `--rezonality` aggregate, goldens, and smoke: pending CI. The Windows `run_process` change (working directory, status, output cap) was checked by reading only.
**Agent documentation and tooling**
- [x] Update product project/compiler boundary notes (`AGENTS.md`).
**Acceptance criteria**
- [x] Production diagnostic decoding is testable without invoking a compiler.
