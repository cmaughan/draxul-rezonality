# Watch all shader include dependencies
**Severity:** HIGH  
**Source:** Codex #21; `plugins/rezonality/src/live_project.cpp:1189`.

The watcher ignores extensions such as `.inc` even though shader compilation accepts them, leaving valid include-only edits unapplied.

**Investigation**

- [ ] Trace dependency discovery, fingerprinting, and rebuild scheduling for direct and nested includes.

**Fix strategy**

- [ ] Watch the actual dependency closure or conservatively cover include files without relying on the current suffix whitelist.
- [ ] Preserve last-valid-generation behavior when an include edit breaks compilation.

**Acceptance criteria**

- [ ] Editing, breaking, and repairing an included `.inc` schedules corresponding generations and diagnostics automatically.
- [ ] Run Rezonality aggregate and real-module edit/recovery coverage, followed by same-cache smoke.
