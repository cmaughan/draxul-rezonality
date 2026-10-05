# Compile shaders under international paths on Windows

**Summary:** Let projects whose paths fall outside the Windows active code page compile instead of failing every shader build.

**Priority:** 14  
**Severity:** HIGH  
**Source:** `plugins/rezonality/src/live_project.cpp`  
**Found during:** `kanban/done/13 international-project-path-serialization -bug.md`

**Evidence and trigger:** `run_process` starts the bundled `tools/win/glslangValidator.exe` with `CreateProcessW`, but the executable imports `_configure_narrow_argv`/`__p___argv`, so the CRT converts its arguments through the active code page. Shader, include, and output paths containing characters the code page cannot represent (for example CJK or emoji under code page 1252) reach the compiler as `?` and the candidate fails with a compile diagnostic. Since card 13 this fails safely and keeps the active scene; it no longer terminates the application. The temporary output directory has the same exposure when the user profile name is not representable.

- [ ] **Investigate:** Confirm on Windows with a non-UTF-8 code page, and choose between a project-relative invocation (child working directory plus relative shader, include, and output paths), a UTF-8-aware compiler build, or an in-process compiler.
- [ ] **Fix:** Compile shaders from accented, Asian, and emoji project paths on Windows while keeping macOS invocation and diagnostic paths unchanged.
- [ ] **Acceptance:** `The staged Rezonality module opens and reloads international project paths` requires `ready` on Windows as well; remove its Windows tolerance.
- [ ] **Validation:** Run the Rezonality-scoped aggregate on Windows and the same-cache smoke.
