# Preserve the latest live diagnostic across late activation

- [x] Reproduce a newer failed build arriving before an older valid candidate
      reaches its first render-thread activation.
- [x] Keep the newer error diagnostic authoritative when that older candidate
      activates or is recreated after a resize.
- [x] Continue configuring the activated generation and publishing presentation
      state without replacing the error JSON.
- [x] Clear the error naturally when a newer successful generation activates.
- [x] Add deterministic coverage for stale and current activation generations.
- [x] Verify the Rezonality aggregate and same-cache Draxul smoke.

## Context

An isolated Metal live-edit run on 2026-09-23 observed generation 2 fail while
generation 1 remained active. The log retained `BUILD FAILED g2`, but the bounded
diagnostics JSON was replaced by `active generation ready` when generation 1
finished its delayed activation. Agent workflows therefore lost the actionable
error even though `attempted_generation` still exceeded `active_generation`.

Activation diagnostics now publish success only when the activated generation
matches the latest attempted generation. A delayed last-good activation still
configures audio and notifies presentation state, while the latest build error
remains available until a genuinely newer generation succeeds.
