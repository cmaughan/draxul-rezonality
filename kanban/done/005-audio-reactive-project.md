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
- [x] Manually verify microphone permission, live response, shared two-pane
      capture, all-hidden suspension, and clean resume on macOS.
- [x] Manually verify microphone permission, live response, shared two-pane
      capture, all-hidden suspension, and clean resume on Windows.

## macOS validation evidence

- 2026-09-23: blessed and visually inspected the deterministic synthetic-audio
  Metal reference on Apple M5; its registered Release comparison passed.
- Focused tests passed for injected permission outcomes, exact-device stream
  sharing, hidden-pane pause/clear/resume, and deterministic FFT data. Physical
  microphone permission, live capture, and shared-device behavior remain manual
  on Windows.

## 2026-09-23 pending-lane verification

- The Debug aggregate passed the deterministic audio shard and synthetic-audio
  Metal render comparison.
- A later isolated macOS run opened two physical-input panes against the same
  default microphone. Both displayed the live spectrum; the user directly
  confirmed the microphone response. A three-second 880 Hz speaker calibration
  tone appeared in both panes, both panes were then hidden for six seconds, and
  the restored tab resumed the shared waveform and spectrum cleanly on the next
  calibration tone. This completes the macOS gate; Windows remains open.

## Windows validation evidence

- An isolated Debug session opened two physical-input panes against the default
  Windows recording device. Both panes displayed the same live spectrum and
  waveform, and the same 880 Hz speaker calibration response appeared in each.
- Minimizing the only attached UI kept both panes hidden for eight seconds.
  The UI remained alive throughout; after restore, the next calibration tone
  immediately restored the shared waveform and spectrum in both panes.
- The aggregate also passed the focused permission, exact-device sharing,
  hidden pause/clear/resume, and deterministic FFT coverage plus the registered
  Vulkan synthetic-audio comparison.
