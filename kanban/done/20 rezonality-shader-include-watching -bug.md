# Watch all shader include dependencies
**Severity:** HIGH  
**Source:** Codex #21; `plugins/rezonality/src/live_project.cpp:1189`.

The watcher ignores extensions such as `.inc` even though shader compilation accepts them, leaving valid include-only edits unapplied.

**Investigation**

- [x] Trace dependency discovery, fingerprinting, and rebuild scheduling for direct and nested includes.

**Fix strategy**

- [x] Watch the actual dependency closure or conservatively cover include files without relying on the current suffix whitelist.
- [x] Preserve last-valid-generation behavior when an include edit breaks compilation.

**Acceptance criteria**

- [x] Editing, breaking, and repairing an included `.inc` schedules corresponding generations and diagnostics automatically in the real-module contract.
- [x] Run Rezonality aggregate and real-module edit/recovery coverage, followed by same-cache smoke.

**Progress (Windows):** The watch fingerprint now hashes all project files,
apart from `.git` metadata, so arbitrary direct or nested include suffixes
trigger the existing debounced candidate build. The CPU live-project test
passed edit/break/repair with a nested `.inc`; the focused project suite passed
11 cases/178 assertions. The Debug all-products aggregate passed 49/49 CTest
entries, including the real compiler/native-module `.inc` edit, failure
diagnostic, and repair regression. Same-cache Debug startup passed via
`py do.py run debug --console -- --smoke-test` (~48 s). The standard 30 s
`smoke --skip-build` wrapper timed out on the existing nine-pane Session, so
that timeout is recorded separately rather than claimed as a pass. This watcher
is shared CPU code; no backend-specific path was changed.
