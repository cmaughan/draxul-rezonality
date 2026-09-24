# Validate procedural surface dimensions before GPU conversion
**Severity:** CRITICAL  
**Source:** Codex #4; `plugins/rezonality/src/native_backend_vulkan.cpp:448` and `native_backend_metal.mm:454`.

A procedural surface with `scale: (1e30, 1)` passes parsing and causes an out-of-range floating-to-integer conversion on both backends.

**Investigation**

- [ ] Trace procedural dimension calculation during activation and pane resize, including device limits.

**Fix strategy**

- [ ] Compute and validate dimensions before casting or allocating on Vulkan and Metal.
- [ ] Return actionable diagnostics while retaining the last valid generation.

**Acceptance criteria**

- [ ] Huge scales and overflow products fail safely; valid scales and resizing remain supported.
- [ ] Run the Rezonality aggregate, relevant render checks, and same-cache smoke; record both-platform evidence.
- [ ] Keep this scope distinct from `kanban/pending/42 rezonality-image-storage-format -bug.md`.
