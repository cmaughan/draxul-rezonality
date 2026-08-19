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
- [ ] Build and bless the corresponding macOS Metal render snapshot.
- [ ] Manually verify microphone permission, live response, shared two-pane
      capture, all-hidden suspension, and clean resume on Windows and macOS.
