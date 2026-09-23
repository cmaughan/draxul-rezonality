# Audio-reactive project

- [x] Port a focused stereo capture, FFT, waveform, and RGBA32F analysis-texture
      path without importing VkLive's audio settings UI.
- [x] Share one SDL recording stream per selected input device across panes.
- [x] Support default input, exact device-name override, deterministic
      synthetic input, and explicit silent fallback through plugin config.
- [x] Pause and clear shared capture when all subscribing panes are hidden, and
      resume it when any subscriber becomes visible.
- [x] Upload analysis generations safely through per-frame Vulkan staging and
      Metal managed textures.
- [x] Stage the VkLive audio spectrum visualizer as a live-editable project.
- [x] Prove deterministic FFT data, visibility behavior, and fallback status.
- [x] Add a conditional Windows Vulkan synthetic-audio render snapshot.
- [x] Build and bless the corresponding macOS Metal render snapshot.
- [ ] Manually verify microphone permission, live response, shared two-pane
      capture, all-hidden suspension, and clean resume on Windows and macOS.

## macOS validation evidence

- 2026-09-23: blessed and visually inspected the deterministic synthetic-audio
  Metal reference on Apple M5; its registered Release comparison passed.
- Focused tests passed for injected permission outcomes, exact-device stream
  sharing, hidden-pane pause/clear/resume, and deterministic FFT data. Physical
  microphone permission, live capture, and shared-device behavior remain manual.

## 2026-09-23 pending-lane verification

- The Debug aggregate passed the deterministic audio shard and synthetic-audio
  Metal render comparison.
- Physical microphone permission and live two-pane capture cannot be established
  by the deterministic test input; the Windows and macOS manual gate remains
  open.
