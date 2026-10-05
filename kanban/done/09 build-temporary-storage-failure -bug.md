# Contain temporary-storage failures during rebuilding

**Summary:** Report unavailable temporary storage so a failed shader edit leaves the application running.

**Priority:** 09  
**Severity:** CRITICAL  
**Source:** `plugins/rezonality/src/live_project.cpp`

**Evidence and trigger:** B07; throwing temporary-directory lookup precedes candidate handling and escapes the unguarded worker build call.

- [x] **Investigate:** Trace all operations outside existing build handlers and worker exception containment.
  Confirmed: `ProjectPipeline::build` called the throwing `fs::temp_directory_path()` outside `build_candidate`'s handler, and `LiveProject::run` called `build` unguarded, so a missing `TMPDIR` aborted the process (reproduced as SIGABRT by the new test before the fix).
- [x] **Fix:** Use error-code lookup and convert complete-build exceptions into ordinary generation diagnostics.
  `src/live_project.cpp`: `ProjectPipeline::build` uses `temp_directory_path(error_code&)` and returns a failed generation; `LiveProject::run` converts any exception from a complete build into a failed `BuildResult`.
- [x] **Acceptance:** Missing temporary storage preserves the active scene, publishes an error, and permits a later successful rebuild.
  Covered by `Rezonality live rebuild reports unavailable temporary storage` (`tests/rezonality_project_tests.cpp`); failed results flow through the existing `RuntimeController::accept` failure path, which keeps the active generation.
- [x] **Validation:** Run the Rezonality-scoped aggregate, relevant reload checks, and same-cache smoke.
