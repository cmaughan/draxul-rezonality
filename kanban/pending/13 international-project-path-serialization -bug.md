# Serialize international project paths safely
**Summary:** Preserve international project names so opening or reloading a graphics project cannot close the application.

**Priority:** 13  
**Severity:** CRITICAL  
**Source:** `plugins/rezonality/src/rezonality_plugin.cpp`  
**Reported by:** Claude C5; consensus F05.

**Evidence and trigger:** Lines 500 and 552 perform narrow path conversions; reload JSON dumping can throw through exported callbacks. Diagnostic identity construction also converts paths without containment. Windows non-UTF-8 code pages expose the failures.

- [ ] **Investigate:** Trace path creation, diagnostic identity, presentation, and reload export/import boundaries.
- [ ] **Fix:** Serialize native paths explicitly as UTF-8 while retaining native filesystem paths internally.
- [ ] **Fix:** Contain failures in affected exported callbacks and preserve the active project on reload failure.
- [ ] **Acceptance:** Projects under accented, Asian, and emoji paths open and reload safely on Windows with a non-UTF-8 code page.
- [ ] **Acceptance:** Presentation and diagnostics remain valid text; existing publication error handling remains effective.
- [ ] **Validation:** Run the Rezonality-scoped aggregate, affected reload checks, and same-cache smoke; preserve macOS path behavior.
