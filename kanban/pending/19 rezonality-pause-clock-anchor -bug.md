# Exclude paused intervals from Rezonality animation time
**Severity:** HIGH  
**Source:** Codex #20; `plugins/rezonality/src/rezonality_plugin.cpp:191`.

Resume leaves the previous render timestamp as the animation anchor, so an idle paused interval is added on the next frame.

**Investigation**

- [ ] Trace animation time through pause, sparse redraws, resume, and state restoration.

**Fix strategy**

- [ ] Reset or reanchor the animation clock at pause/resume transitions.

**Acceptance criteria**

- [ ] Long pauses cause no animation jump, with or without incidental paused redraws.
- [ ] Verify both backends and reload behavior; run Rezonality aggregate tests, relevant render checks, and same-cache smoke.
