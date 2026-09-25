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
- [ ] Confirm Metal resume/reload rendering on macOS.

**Progress (Windows):** `AnimationClock` is shared by Vulkan and Metal frame
callbacks. Pause/resume invalidates its timestamp anchor; restored state also
starts with a fresh anchor. The deterministic focused clock case passed nine
assertions covering paused frames, no paused frames, resume, and restore. The
plugin/Vulkan native contract target compiled. The Debug all-products aggregate
passed 49/49 CTest entries, including Rezonality render snapshots; same-cache
Debug startup passed via `py do.py run debug --console -- --smoke-test`
(~48 s). The fixed 30 s `smoke --skip-build` wrapper timed out on the
existing nine-pane Session. macOS Metal resume/reload rendering remains open.
