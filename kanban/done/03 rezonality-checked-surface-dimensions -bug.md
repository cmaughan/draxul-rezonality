# Validate procedural surface dimensions before GPU conversion
**Severity:** CRITICAL  
**Source:** Codex #4; `plugins/rezonality/src/native_backend_vulkan.cpp:448` and `native_backend_metal.mm:454`.

A procedural surface with `scale: (1e30, 1)` passes parsing and causes an out-of-range floating-to-integer conversion on both backends.

**Investigation**

- [x] Trace procedural dimension calculation during activation and pane resize, including device limits.

**Fix strategy**

- [x] Compute and validate dimensions before casting or allocating on Vulkan and Metal.
- [x] Return actionable diagnostics while retaining the last valid generation.

**Acceptance criteria**

- [x] Huge scales and overflow products fail safely; valid scales and resizing remain supported.
- [x] Run the Rezonality aggregate, relevant render checks, and same-cache smoke.
- [x] Confirm the Metal path on macOS (build and render/reload smoke).
- [x] Keep this scope distinct from `kanban/pending/42 rezonality-image-storage-format -bug.md`.

**Progress (Windows):** Both native backends call the shared checked-dimension
calculation before integer conversion or allocation. Vulkan uses
`maxImageDimension2D` (also bounded by the attachment API's signed-int width);
Metal uses a conservative family-based 2D texture limit. Backend preparation
rejects an oversized candidate and the runtime retains the prior generation.
The focused project suite passed 11 cases/178 assertions, including huge scales,
oversized fixed images, valid scales, and pane-size calculations. Vulkan plugin
and native contract targets compiled. The Debug all-products aggregate passed
49/49 CTest entries, including the exact `scale: (1e30, 1)` scene repro and
Rezonality render snapshots. Same-cache Debug startup passed via
`py do.py run debug --console -- --smoke-test` (~48 s); the standard 30 s
`smoke --skip-build` wrapper timed out on the existing nine-pane Session, so
that timeout is not counted as a pass. macOS Metal execution remains open.
Image storage validation is unchanged.

**macOS Metal gate (2026-09-26):** The Debug app and Rezonality plugin built,
the all-products unit inventory passed 47/47 CTest entries, and all seven
Rezonality Metal render snapshots passed. A live render-test project began with
generation 1 visible; changing its surface scale to `(1e30, 1)` produced
`BUILD FAILED g2 | rendering last good g1` with the Metal device's 16384-wide
limit. Restoring a valid scene and changing its shader color produced a green
final Metal frame without restarting the app. This exercised rejection,
last-good retention, and successful reload on the native backend.
