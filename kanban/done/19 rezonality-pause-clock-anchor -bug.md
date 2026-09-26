# Exclude paused intervals from Rezonality animation time
**Severity:** HIGH  
**Source:** Codex #20; `plugins/rezonality/src/rezonality_plugin.cpp:191`.

Resume leaves the previous render timestamp as the animation anchor, so an idle paused interval is added on the next frame.

**Investigation**

- [x] Trace animation time through pause, sparse redraws, resume, and state restoration.

**Fix strategy**

- [x] Reset or reanchor the animation clock at pause/resume transitions.

**Acceptance criteria**

- [x] Long pauses cause no animation jump, with or without incidental paused redraws.
- [x] Run Rezonality aggregate, relevant render checks, and same-cache smoke.
- [x] Confirm Metal resume/reload rendering on macOS.

**Progress (Windows):** `AnimationClock` is shared by Vulkan and Metal frame
callbacks. Pause/resume invalidates its timestamp anchor; restored state also
starts with a fresh anchor. The deterministic focused clock case passed nine
assertions covering paused frames, no paused frames, resume, and restore. The
plugin/Vulkan native contract target compiled. The Debug all-products aggregate
passed 49/49 CTest entries, including Rezonality render snapshots; same-cache
Debug startup passed via `py do.py run debug --console -- --smoke-test`
 (~48 s). The fixed 30 s `smoke --skip-build` wrapper timed out on the
existing nine-pane Session. Metal resume/reload rendering was checked later below.

**macOS partial gate (2026-09-26):** The Debug Metal plugin built, all seven
Rezonality Metal render snapshots passed, and a live shader-project reload
rejected an oversized candidate while retaining the last good frame before
recovering to a changed shader. Native pause/resume input and elapsed-time
continuity needed a direct Metal check, which was completed below.

**macOS completed gate (2026-09-26):** Ran the native Metal render-test window
twice against a temporary copy of `examples/simple`, with a fragment shader that
encodes animation time in the output red channel. Both 14-second runs rendered
and exited successfully. The uninterrupted frame measured red=128; sending
Space to pause at 3 seconds and resume at 9 seconds measured red=64. The
six-second paused interval therefore did not advance the animation clock. The
live reload path was separately validated on Metal earlier that day. All
acceptance criteria are now checked.
