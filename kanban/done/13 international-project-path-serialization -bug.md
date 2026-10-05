# Serialize international project paths safely
**Summary:** Preserve international project names so opening or reloading a graphics project cannot close the application.

**Priority:** 13  
**Severity:** CRITICAL  
**Source:** `plugins/rezonality/src/rezonality_plugin.cpp`  
**Reported by:** Claude C5; consensus F05.

**Evidence and trigger:** Lines 500 and 552 perform narrow path conversions; reload JSON dumping can throw through exported callbacks. Diagnostic identity construction also converts paths without containment. Windows non-UTF-8 code pages expose the failures.

- [x] **Investigate:** Trace path creation, diagnostic identity, presentation, and reload export/import boundaries.
  Narrow `path::string()`/`generic_string()` conversions (Windows active code page; throw on unmappable characters, or yield non-UTF-8 bytes that make `json::dump` throw) were in presentation status, reload export/import, `diagnostic_stage`, runtime failure status, diagnostics identity/path/temporary-file naming, the project-missing error in `parse_project_options` (outside its handler, so it escaped `create_instance`), project fingerprinting, active-source collection, the compiler `-I` argument, and image/model loading. macOS paths are native UTF-8 and were unaffected.
- [x] **Fix:** Serialize native paths explicitly as UTF-8 while retaining native filesystem paths internally.
  New `src/path_utf8.h` (`path_utf8`, `generic_path_utf8`, non-throwing `display_path_utf8`) replaces the narrow conversions; the compiler include argument and diagnostics temporary file are built by native path concatenation; Assimp receives UTF-8 and stb_image is built with `STBI_WINDOWS_UTF8`. ASCII and macOS identities, fingerprints, and serialized values are byte-for-byte unchanged.
- [x] **Fix:** Contain failures in affected exported callbacks and preserve the active project on reload failure.
  `create_instance` and `export_reload_json` no longer let exceptions cross the C ABI; `import_reload_json` was already contained. Build failures continue through the failed-candidate path that keeps the active generation.
- [x] **Acceptance:** Projects under accented, Asian, and emoji paths open and reload safely on Windows with a non-UTF-8 code page.
  Covered by `The staged Rezonality module opens and reloads international project paths` (requires `ready` on macOS). On Windows the bundled narrow-argv glslangValidator cannot open names outside the code page, so those candidates fail safely with a diagnostic; tracked by `kanban/pending/14 windows-shader-compiler-unicode-paths -bug.md`. Not executed on Windows locally.
- [x] **Acceptance:** Presentation and diagnostics remain valid text; existing publication error handling remains effective.
  The test checks UTF-8 presentation status, an ASCII default diagnostics identity, the published UTF-8 `project_path`, and export/import round-trip of the UTF-8 path.
- [x] **Validation:** Run the Rezonality-scoped aggregate, affected reload checks, and same-cache smoke; preserve macOS path behavior.
